#include "session/remote_video_engine.h"

#include "app/runtime_config.h"
#include "capture/capture_source_factory.h"
#include "capture/frame_sanitizer.h"
#include "capture/process_ui_capture.h"
#include "common/rpc_time.h"
#include "common/window_ops.h"
#include "common/character_conversion.h"
#include "encode/video_encode_pipeline.h"
#include "input/input_controller.h"
#include "common/process_ops.h"
#include "session/session_health_policy.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <thread>
#include <utility>

#include <windows.h>

namespace {

BmpDumpDiag make_bmp_dump_diag(const CaptureGrabOutcome& o)
{
    BmpDumpDiag d;
    d.use_hw_capture = o.used_hw_capture;
    d.force_software_active = false;
    d.top_black_strip_streak = 0;
    d.dxgi_instability_score = 0;
    d.dxgi_disabled_for_session = false;
    return d;
}

HWND primary_hwnd_from_surfaces(const std::vector<window_ops::window_info>& surfaces)
{
    if (surfaces.empty()) return nullptr;
    HWND best = nullptr;
    int best_area = -1;
    for (const auto& s : surfaces) 
    {
        const int w = s.rect_screen.right - s.rect_screen.left;
        const int h = s.rect_screen.bottom - s.rect_screen.top;
        const int a = w * h;
        if (a > best_area) 
        {
            best_area = a;
            best = s.hwnd;
        }
    }
    return best;
}

} // namespace

remote_video_engine::remote_video_engine(const std::string& exe_path, std::function<void()> on_remote_process_exit, remote_video_engine::window_missing_fn on_window_missing)
    : m_on_remote_process_exit(std::move(on_remote_process_exit))
    , m_on_window_missing(std::move(on_window_missing))
{
    m_process_ops = std::make_unique<process_ops>(exe_path);

    m_video_encode_pipeline = std::make_unique<VideoEncodePipeline>();
    m_bmp_dump.configure_from_config();

    m_window_missing_exit_grace_ms = 5000;
    m_video_fps = (std::max)(1, runtime_config::get_int("RPC_ACTIVE_FPS", 30));
    m_video_encode_pipeline->configure(m_video_fps, m_encoder_layout_change_threshold_px, m_encoder_layout_change_required_streak);

    m_ui_capture_options = ProcessUiCapture::load_layout_options_from_config();

    const std::string backend_cfg =
        to_lower_ascii(runtime_config::get_string("RPC_CAPTURE_BACKEND", "auto"));
    const CaptureKindResolveResult resolved = resolve_capture_kind(backend_cfg);
    m_capture_explicit_backend_error = resolved.explicit_backend_unavailable;
    if (m_capture_explicit_backend_error) {
        m_capture_kind = ProcessCaptureKind::Gdi;
        std::cout << "[capture] RPC_CAPTURE_BACKEND=" << backend_cfg
                  << " unavailable at init; strict mode — no GDI fallback, capture backend not created.\n";
        m_steady_frame_hold = true;
        m_capture_source.reset();
    } else {
        m_capture_kind = resolved.kind;
        m_steady_frame_hold = (m_capture_kind == ProcessCaptureKind::Gdi);
        m_capture_source = create_capture_source(m_capture_kind);
        if (m_capture_source) {
            m_capture_source->init();
        }
    }

    m_latest_encoded.capacity = (size_t)(std::max)(1, runtime_config::get_int("RPC_SEND_QUEUE_DEPTH", 1));
}

bool remote_video_engine::is_remote_process_still_running() const
{
    // Prefer the original launch handle when available.
    if (m_pi.hProcess) {
        DWORD exit_code = 0;
        if (m_process_ops->get_exit_code(m_pi.hProcess, exit_code) && exit_code == STILL_ACTIVE) {
            return true;
        }
    }
    const DWORD cap = m_capture_pid;
    const DWORD launch = m_launch_pid;
    if (m_process_ops != 0 && m_process_ops->is_running(cap)) return true;
    if (launch != 0 && m_process_ops->is_running(launch)) return true;
    return false;
}

void remote_video_engine::notify_window_missing_if_needed(const char* why, uint64_t now_unix_ms)
{
    // Throttle to avoid spamming front-end; default 2s.
    const uint64_t throttle_ms = 2000;
    if (m_last_window_missing_notify_unix_ms != 0 &&
        now_unix_ms >= m_last_window_missing_notify_unix_ms &&
        (now_unix_ms - m_last_window_missing_notify_unix_ms) < throttle_ms) {
        return;
    }
    m_last_window_missing_notify_unix_ms = now_unix_ms;

    if (!m_on_window_missing) return;
    const uint64_t since = m_window_missing_since_unix_ms ? m_window_missing_since_unix_ms : now_unix_ms;
    const uint64_t missing_ms = (now_unix_ms >= since) ? (now_unix_ms - since) : 0;
    try {
        m_on_window_missing(why ? why : "", missing_ms);
    } catch (...) {
    }
}

remote_video_engine::~remote_video_engine()
{
    stop();
}

void remote_video_engine::start()
{    
    if (!m_process_ops) return;

    if (m_running.exchange(true)) return;

    if (m_capture_explicit_backend_error) {
        const std::string req = to_lower_ascii(runtime_config::get_string("RPC_CAPTURE_BACKEND", ""));
        std::cout << "[capture] abort session start: explicit RPC_CAPTURE_BACKEND=" << req
                  << " unavailable (strict, no GDI fallback). Set RPC_CAPTURE_BACKEND=auto or gdi.\n";
        m_running = false;
        return;
    }

    m_main_window = nullptr;

    reset_for_session_start();

    m_pi = {};
    m_launch_pid = 0;
    m_capture_pid = 0;
    m_main_window = nullptr;
    if (!launch_attached_remote_process()) {
        m_running = false;
        return;
    }
    m_pid_rebind_deadline_unix_ms = rpc_unix_epoch_ms() + 5000;

    m_ui_capture_options = ProcessUiCapture::load_layout_options_from_config();

    std::cout << "[capture] capture_kind=" << static_cast<int>(m_capture_kind)
              << " layout=" << static_cast<int>(m_ui_capture_options.composite_layout) << "\n";

    m_exit_notified = false;
    m_exit_watch_thread = std::thread(&remote_video_engine::exit_watch_loop, this);

    // Capture/encode pipeline threads
    m_threads_running.store(true, std::memory_order_release);
    m_capture_thread = std::thread(&remote_video_engine::capture_loop, this);
    m_encode_thread = std::thread(&remote_video_engine::encode_loop, this);
}

void remote_video_engine::stop()
{
    m_running = false;
    m_threads_running.store(false, std::memory_order_release);

    // Wake encode thread if waiting.
    {
        std::lock_guard<std::mutex> lk(m_latest_frame.mtx);
        m_latest_frame.cv.notify_all();
    }

    if (m_exit_watch_thread.joinable()) {
        m_exit_watch_thread.join();
    }
    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
    }
    if (m_encode_thread.joinable()) {
        m_encode_thread.join();
    }

    m_last_launch_placement_hwnd = nullptr;

    if (m_capture_source) {
        m_capture_source->shutdown();
    }

    input_controller::instance()->set_capture_screen_rect(0, 0, 0, 0);

    try {
        if (m_process_ops && m_pi.hProcess) {
            std::cout << "[proc] stop: terminating processes launch_pid=" << m_launch_pid
                      << " capture_pid=" << m_capture_pid << std::endl;
            m_process_ops->terminate_detached_launch(m_pi, m_capture_pid, m_launch_pid, 0);
        }
    } catch (...) {
    }
}

void remote_video_engine::notify_remote_exit_if_needed(const char* why)
{
    if (m_exit_notified.exchange(true)) return;
    try {
        std::cout << "[proc] remote exit notify why=" << (why ? why : "")
                  << " launch_pid=" << m_launch_pid
                  << " capture_pid=" << m_capture_pid
                  << " main_hwnd=" << static_cast<void*>(m_main_window)
                  << std::endl;
    } catch (...) {
    }
    if (m_on_remote_process_exit) {
        try {
            m_on_remote_process_exit();
        } catch (...) {
        }
    }
}

void remote_video_engine::exit_watch_loop()
{
    while (m_running.load(std::memory_order_relaxed)) {
        if (m_main_window) {
            window_ops wops;
            if (wops.is_valid(m_main_window)) {
                DWORD  window_owner_pid = wops.get_window_pid(m_main_window);

                if (window_owner_pid != 0) {
                    auto h = m_process_ops->open_process(window_owner_pid, PROCESS_QUERY_LIMITED_INFORMATION);//主窗口所属 PID（window_owner_pid）为主判断
                    if (!h) {
                        notify_remote_exit_if_needed("main_window_owner_open_failed");
                        break;
                    }
                    DWORD exit_code = 0;
                    if (!m_process_ops->get_exit_code(h.get(), exit_code)) {
                        notify_remote_exit_if_needed("main_window_owner_exit_code_failed");
                        break;
                    }
                    if (exit_code != STILL_ACTIVE) {
                        notify_remote_exit_if_needed("main_window_owner_exited");
                        break;
                    }
                }
                else if (m_pi.hProcess) {
                    DWORD exit_code = 0;
                    const bool ok = m_process_ops->get_exit_code(m_pi.hProcess, exit_code);
                    if (ok && exit_code != STILL_ACTIVE) {
                        const DWORD cap = m_capture_pid;
                        const DWORD launch = m_launch_pid;
                        const bool capturing_child = (cap != 0 && cap != launch && m_process_ops->is_running(cap));
                        const bool still_in_rebind_grace = rpc_unix_epoch_ms() <= m_pid_rebind_deadline_unix_ms + 3000;
                        if (capturing_child || still_in_rebind_grace) {
                        }
                        else {
                            notify_remote_exit_if_needed("launch_process_exited");
                            break;
                        }
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void remote_video_engine::apply_emit_fail_policy(rtc::binary& out_sample, const bool request_idr)
{
    if (m_steady_frame_hold && m_have_last_good_sample.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(m_last_good_sample_mtx);
        out_sample = m_last_good_video_sample;
    } else {
        out_sample.clear();
    }
    if (request_idr && m_have_last_good_sample.load(std::memory_order_relaxed)) {
        request_force_keyframe();
    }
}

//采集生产者,不断抓取进程 UI 画面（DXGI/GDI）并合成 RGB 帧，写入最新帧槽位。
void remote_video_engine::capture_loop()
{
    // Local state for frame sanitization; capture thread owns rgb history.
    std::vector<uint8_t> last_good_rgb;
    int last_good_w = 0;
    int last_good_h = 0;
    bool had_successful_video = false;

    // pacing
    const int fps = (std::max)(1, m_video_fps);
    const auto frame_period = std::chrono::microseconds(1000000 / fps);
    auto next_tick = std::chrono::steady_clock::now();

    while (m_threads_running.load(std::memory_order_acquire)) {
        const auto now_unix_ms = rpc_unix_epoch_ms();

        if (!m_process_ops || !m_capture_source) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

		// Re-validate main window each tick to handle dynamic surface changes; if lost, attempt recovery and health check.
        window_ops wops;
        const std::vector<window_ops::window_info> surfaces = wops.enumerate_visible_top_level(m_capture_pid);

        if (surfaces.empty()) {
            m_main_window = nullptr;
			try_recover_main_window();//尝试恢复主窗口：先按 PID 找，启动窗口期内按 exe 名重绑。
            const bool notify = SessionHealthPolicy::should_notify_remote_exit(m_had_successful_video.load(std::memory_order_relaxed), now_unix_ms, m_window_missing_since_unix_ms, m_window_missing_exit_grace_ms);
            if (notify) {
                if (is_remote_process_still_running()) {
                    notify_window_missing_if_needed("no_surfaces_grace_expired_but_process_alive", now_unix_ms);
                } else {
                    notify_remote_exit_if_needed("no_surfaces_grace_expired");
                    m_running = false;
                    m_threads_running.store(false, std::memory_order_release);
                }
            }
            std::cout << "[capture] no surfaces found for pid=" << m_capture_pid
                      << " main_window=" << static_cast<void*>(m_main_window)
                      << " notify_remote_exit_if_needed=" << notify
				<< std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        m_main_window = primary_hwnd_from_surfaces(surfaces);//选主窗口,以最大面积者优先
        apply_launch_window_placement(m_main_window);//可能执行一次窗口摆放

        // Capture and compose.
        const uint64_t prep_unix_ms = rpc_unix_epoch_ms();
        CaptureGrabOutcome outcome = ProcessUiCapture::grab_process_ui_rgb(m_capture_pid, surfaces, m_ui_capture_options, *m_capture_source, now_unix_ms);

        m_last_capture_used_hw = outcome.used_hw_capture;

        if (!outcome.ok || outcome.frame.empty() || outcome.width <= 0 || outcome.height <= 0) {
            // No new frame; pacing continues.
        } else {
            // Sanitize in capture thread; do not treat drops as session health failures.
            const bool keep = FrameSanitizer::sanitize_frame(outcome.frame, outcome.width, outcome.height, had_successful_video, !last_good_rgb.empty(), last_good_rgb, last_good_w, last_good_h);

            if (keep) {
                // Absolute unix-ms timestamp at "capture-complete" moment (used for per-frame SEI timestamps).
                const uint64_t cap_done_unix_ms = rpc_unix_epoch_ms();

                input_controller::instance()->set_capture_screen_rect(outcome.cap_min_left, outcome.cap_min_top, outcome.width, outcome.height);

                const BmpDumpDiag bmp_diag = make_bmp_dump_diag(outcome);
                m_bmp_dump.dump_capture_if_needed(outcome.frame, outcome.width, outcome.height, bmp_diag);

                // Update sanitizer history before moving outcome.frame away.
                last_good_rgb = outcome.frame; // copy; can be optimized later with buffer reuse.
                last_good_w = outcome.width;
                last_good_h = outcome.height;
                had_successful_video = true;

                // Store latest frame with overwrite semantics.
                CapturedFrame cf;
                cf.frame_id = m_frame_id_seq.fetch_add(1, std::memory_order_relaxed) + 1;
                cf.unix_ms = cap_done_unix_ms;
                cf.prep_unix_ms = prep_unix_ms;
				cf.grab_outcome = outcome;
				cf.grab_outcome.frame = std::move(outcome.frame); // ensure frame data moves with the outcome for potential diagnostics.

                {
                    std::lock_guard<std::mutex> lk(m_latest_frame.mtx);
                    if (m_latest_frame.latest.has_value()) {
                        m_latest_frame.dropped_by_overwrite++;
                    }
                    m_latest_frame.latest = std::move(cf);
                    m_latest_frame.stored_frames++;
                }
                m_latest_frame.cv.notify_one();
            }
        }

        // Pacing.
        next_tick += frame_period;
        const auto now = std::chrono::steady_clock::now();
        if (next_tick > now) {
            std::this_thread::sleep_for(next_tick - now);
        } else {
            // If we're behind, skip ahead to avoid drift.
            next_tick = now;
        }
    }
}

//编码生产者/样本生产者, 等待最新帧槽位有新帧（或超时），取出后编码并推入最新编码队列。
void remote_video_engine::encode_loop()
{
    uint64_t last_encoded_id = 0;

    while (m_threads_running.load(std::memory_order_acquire)) {
        std::optional<CapturedFrame> frame;
        {
            //1.等待新帧（最多 50ms）
            std::unique_lock<std::mutex> lk(m_latest_frame.mtx);
            m_latest_frame.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !m_threads_running.load(std::memory_order_acquire) || (m_latest_frame.latest.has_value() && m_latest_frame.latest->frame_id != last_encoded_id);
            });
            if (!m_threads_running.load(std::memory_order_acquire)) break;
            if (!m_latest_frame.latest.has_value()) continue;
            if (m_latest_frame.latest->frame_id == last_encoded_id) continue;
            
            //2.“拿走最新帧”并立即释放锁Take a snapshot of the latest (copy/move rgb to avoid holding lock during encode).
            frame = std::move(m_latest_frame.latest.value());
            // Keep the slot populated with an empty placeholder so overwrite count remains meaningful.
            m_latest_frame.latest.reset();
        }

        //3.确认编码管线存在
        if (!frame.has_value() || !m_video_encode_pipeline) continue;

		if (m_latest_encoded.q.size() > 3) continue;//如果编码队列里积压了过多样本，先不编码新帧了，等消费者把队列吃掉一些再说。这是为了防止编码过慢时���存占用持续增长。
        CapturedFrame& cf = frame.value();
        last_encoded_id = cf.frame_id;

        int target_w = cf.grab_outcome.width;
        int target_h = cf.grab_outcome.height;
        bool applied_layout = false;
        //4.确保编码器布局/分辨率策略一致
        const bool layout_ok = m_video_encode_pipeline->ensure_encoder_layout( cf.grab_outcome.width, cf.grab_outcome.height, m_had_successful_video.load(std::memory_order_relaxed), target_w, target_h, applied_layout);
        if (!layout_ok) continue;

        //5.编码
        VideoEncodeResult enc = m_video_encode_pipeline->encode_frame(cf.grab_outcome.frame, cf.grab_outcome.width, cf.grab_outcome.height, applied_layout);

        if (enc.encode_ok && !enc.sample.empty() && !enc.invalid_payload) {
            EncodedSample es;
            es.frame_id = cf.frame_id;
            es.cap_unix_ms = cf.unix_ms;
            es.prep_unix_ms = cf.prep_unix_ms;
            es.unix_ms = enc.frame_unix_ms ? enc.frame_unix_ms : cf.unix_ms;
            es.sample = std::move(enc.sample);
            es.used_hw_capture = cf.grab_outcome.used_hw_capture;
            es.w = target_w;
            es.h = target_h;

            {
                //5.1 推入最新编码队列
                std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
                while (m_latest_encoded.q.size() >= m_latest_encoded.capacity) {
                    m_latest_encoded.q.pop_front();
                    m_latest_encoded.dropped_by_overflow++;
                }
                m_latest_encoded.q.push_back(es);
                m_latest_encoded.pushed++;
            }

            {
                //更新编码遥测快照
                std::lock_guard<std::mutex> lk(m_last_enc_mtx);
                m_last_capture_unix_ms = es.cap_unix_ms;
                m_last_prep_unix_ms = es.prep_unix_ms;
                m_last_frame_unix_ms = es.unix_ms;
                m_last_capture_w = es.w;
                m_last_capture_h = es.h;
            }

            // Update last good sample for steady-hold mode.
            {
                //更新 steady-hold 所需的“最后一份好样本”
                std::lock_guard<std::mutex> lk(m_last_good_sample_mtx);
                m_last_good_video_sample = es.sample;
            }
            m_have_last_good_sample.store(true, std::memory_order_relaxed);
            m_had_successful_video.store(true, std::memory_order_relaxed);
        } else {
            // If encode fails but we already have a good sample, request keyframe to recover quickly.
            if (m_have_last_good_sample.load(std::memory_order_relaxed))  request_force_keyframe();
        }
    }
}

//MediaSession 调度线程，样本消费者,每个视频 tick 来“取一次样本”，把样本交给 sender（外部调用它的人会把 out_sample 传给 m_sender->on_video_sample(...)）。
void remote_video_engine::produce_next_video_sample(rtc::binary& out_sample, remote_capture_telemetry& out_telemetry)
{
    out_sample.clear();
    out_telemetry = remote_capture_telemetry{};

    //1.写入基础遥测
    const uint64_t now_unix_ms = rpc_unix_epoch_ms();
    out_telemetry.last_frame_unix_ms = now_unix_ms;
    out_telemetry.last_capture_used_hw = m_last_capture_used_hw;

    //2.无阻塞地“取走最新编码样本”Pop latest encoded sample (no blocking). If none, apply steady-hold fallback policy.
    EncodedSample es;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
        if (!m_latest_encoded.q.empty()) {
            es = std::move(m_latest_encoded.q.front());
			m_latest_encoded.q.pop_front();

            //es = std::move(m_latest_encoded.q.back());
            //m_latest_encoded.q.clear();
            have = true;
        }
    }

    //3.若拿到了有效样本：填充输出并返回
    if (have && !es.sample.empty()) {
        out_sample = std::move(es.sample);
        //把编码线程产出的遥测写到
        out_telemetry.last_capture_unix_ms = es.cap_unix_ms;
        out_telemetry.last_encode_unix_ms = es.unix_ms;
        out_telemetry.last_prep_unix_ms = es.prep_unix_ms;
        out_telemetry.last_frame_unix_ms = es.unix_ms ? es.unix_ms : now_unix_ms;
        out_telemetry.capture_width = es.w;
        out_telemetry.capture_height = es.h;

        return;
    }

    // 否则：走“稳帧兜底（steady-hold）”并用上一次遥测,No fresh sample; reuse last good sample if configured.
    apply_emit_fail_policy(out_sample, false);
    {
        //加锁 m_last_enc_mtx，用上一次成功编码的快照填 out_telemetry：
        std::lock_guard<std::mutex> lk(m_last_enc_mtx);
        out_telemetry.last_capture_unix_ms = m_last_capture_unix_ms;
        out_telemetry.last_encode_unix_ms = m_last_frame_unix_ms;
        out_telemetry.last_prep_unix_ms = m_last_prep_unix_ms;
        out_telemetry.last_frame_unix_ms = m_last_frame_unix_ms ? m_last_frame_unix_ms : now_unix_ms;
        out_telemetry.capture_width = m_last_capture_w;
        out_telemetry.capture_height = m_last_capture_h;
    }
}

void remote_video_engine::request_force_keyframe()
{
    if (!m_video_encode_pipeline) return;
    try {
        m_video_encode_pipeline->request_force_keyframe_with_cooldown(rpc_unix_epoch_ms());
    } catch (...) {
    }
}

HWND remote_video_engine::get_main_window() const
{
    return m_main_window;
}

void remote_video_engine::reset_for_session_start()
{
    m_last_launch_placement_hwnd = nullptr;

    m_had_successful_video.store(false, std::memory_order_relaxed);
    m_last_capture_unix_ms = 0;
    m_last_prep_unix_ms = 0;
    m_last_frame_unix_ms = 0;
    {
        std::lock_guard<std::mutex> lk(m_last_good_sample_mtx);
        m_last_good_video_sample.clear();
    }
    m_have_last_good_sample.store(false, std::memory_order_relaxed);

    m_last_good_rgb_frame.clear();
    m_last_good_rgb_w = 0;
    m_last_good_rgb_h = 0;

    m_window_missing_since_unix_ms = 0;

    if (m_video_encode_pipeline) m_video_encode_pipeline->reset_for_stream_start();
    if (m_capture_source) m_capture_source->reset_session_recovery();
    m_bmp_dump.reset_session();
}

void remote_video_engine::try_recover_main_window()
{
    if (!m_process_ops) return;

    if (m_main_window && SessionHealthPolicy::is_window_viable_for_capture(m_main_window)) return;
    m_main_window = nullptr;

    std::string exe_base_name = m_process_ops->target_exe_base_name_lower();
    if (exe_base_name.empty()) return;

    m_main_window = SessionHealthPolicy::try_recover_main_window(
        *m_process_ops,
        m_launch_pid,
        m_capture_pid,
        exe_base_name,
        true,
        m_had_successful_video.load(std::memory_order_relaxed),
        m_pid_rebind_deadline_unix_ms);

    apply_launch_window_placement(m_main_window);
}

bool remote_video_engine::launch_attached_remote_process()
{
    if (!m_process_ops) return false;
    if (!m_process_ops->start()) return false;

    m_launch_pid = m_process_ops->launch_pid();
    m_capture_pid = m_process_ops->capture_pid();
    m_process_ops->detach_launch_process_info(m_pi);

    // 不要因等待窗口而阻塞流创建。
    m_main_window = SessionHealthPolicy::find_main_window(m_launch_pid);
    return true;
}

void remote_video_engine::apply_launch_window_placement(HWND hwnd)
{
    window_ops wops;
    if (!wops.is_valid(hwnd)) return;
    if (hwnd == m_last_launch_placement_hwnd) return;

    (void)AllowSetForegroundWindow(ASFW_ANY);

    //ShowWindow(hwnd, SW_MAXIMIZE);
    //SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    m_last_launch_placement_hwnd = hwnd;
    std::cout << "[proc_ui] launch_window_placement hwnd=" << static_cast<void*>(hwnd) << std::endl;
}
