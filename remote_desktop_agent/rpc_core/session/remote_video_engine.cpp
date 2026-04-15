#include "session/remote_video_engine.h"

#include "session/remote_process_session.h"

#include "app/runtime_config.h"
#include "capture/capture_init_context.h"
#include "capture/capture_source_factory.h"
#include "capture/frame_sanitizer.h"
#include "capture/process_ui_capture.h"
#include "common/rpc_time.h"
#include "common/window_ops.h"
#include "encode/video_encode_pipeline.h"
#include "input/input_controller.h"
#include "common/process_ops.h"
#include "session/session_health_policy.h"

#include <chrono>
#include <iostream>
#include <algorithm>
#include <thread>
#include <utility>

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

remote_video_engine::remote_video_engine(std::string exe_path,
                                         std::function<void()> on_remote_process_exit,
                                         remote_video_engine::window_missing_fn on_window_missing)
    : m_exe_path(std::move(exe_path))
    , m_on_remote_process_exit(std::move(on_remote_process_exit))
    , m_on_window_missing(std::move(on_window_missing))
{
    m_process_session = std::make_unique<RemoteProcessSession>();

    m_video_encode_pipeline = std::make_unique<VideoEncodePipeline>();
    m_bmp_dump.configure_from_config();

    m_window_missing_exit_grace_ms = 5000;
    m_video_fps = (std::max)(1, runtime_config::get_int("RPC_ACTIVE_FPS", 30));
    m_video_encode_pipeline->configure(m_video_fps, m_encoder_layout_change_threshold_px, m_encoder_layout_change_required_streak);

    m_ui_capture_options = ProcessUiCapture::load_layout_options_from_config();

    const std::string backend_cfg =
        ProcessUiCapture::to_lower_ascii(runtime_config::get_string("RPC_CAPTURE_BACKEND", "auto"));
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
            CaptureInitContext init_ctx{};
            m_capture_source->init(init_ctx);
        }
    }

    m_latest_encoded.capacity = (size_t)(std::max)(1, runtime_config::get_int("RPC_SEND_QUEUE_DEPTH", 1));
}

bool remote_video_engine::is_remote_process_still_running() const
{
    // Prefer the original launch handle when available.
    if (m_pi.hProcess) {
        process_ops ops;
        DWORD exit_code = 0;
        if (ops.get_exit_code(m_pi.hProcess, exit_code) && exit_code == STILL_ACTIVE) {
            return true;
        }
    }
    const DWORD cap = m_capture_pid;
    const DWORD launch = m_launch_pid;
    process_ops ops;
    if (cap != 0 && ops.is_running(cap)) return true;
    if (launch != 0 && ops.is_running(launch)) return true;
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
    if (m_running.exchange(true)) return;

    if (!m_process_session) return;

    if (m_capture_explicit_backend_error) {
        const std::string req = ProcessUiCapture::to_lower_ascii(runtime_config::get_string("RPC_CAPTURE_BACKEND", ""));
        std::cout << "[capture] abort session start: explicit RPC_CAPTURE_BACKEND=" << req
                  << " unavailable (strict, no GDI fallback). Set RPC_CAPTURE_BACKEND=auto or gdi.\n";
        m_running = false;
        return;
    }

    m_capture_pid = 0;
    m_launch_pid = 0;
    m_main_window = nullptr;

    reset_for_session_start();

    std::string target_base;
    m_pi = {};
    m_launch_pid = 0;
    m_capture_pid = 0;
    m_main_window = nullptr;
    if (!m_process_session->launch_process(m_exe_path, m_pi, m_launch_pid, m_capture_pid, m_main_window, target_base)) {
        m_running = false;
        return;
    }
    m_target_exe_base_name = target_base;
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
        if (m_process_session && m_pi.hProcess) {
            std::cout << "[proc] stop: terminating processes launch_pid=" << m_launch_pid
                      << " capture_pid=" << m_capture_pid << std::endl;
            m_process_session->terminate_processes(m_pi, m_capture_pid, m_launch_pid);
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
        DWORD window_owner_pid = 0;
        HWND mw = m_main_window;
        if (mw) {
            window_ops wops;
            if (wops.is_valid(mw)) {
                window_owner_pid = wops.get_window_pid(mw);
            }
        }

        if (window_owner_pid != 0) {
            process_ops ops;
            auto h = ops.open_process(window_owner_pid, PROCESS_QUERY_LIMITED_INFORMATION);
            if (!h) {
                notify_remote_exit_if_needed("main_window_owner_open_failed");
                break;
            }
            DWORD exit_code = 0;
            if (!ops.get_exit_code(h.get(), exit_code)) {
                notify_remote_exit_if_needed("main_window_owner_exit_code_failed");
                break;
            }
            if (exit_code != STILL_ACTIVE) {
                notify_remote_exit_if_needed("main_window_owner_exited");
                break;
            }
        } else if (m_pi.hProcess) {
            DWORD exit_code = 0;
            process_ops ops;
            const bool ok = ops.get_exit_code(m_pi.hProcess, exit_code);
            if (ok && exit_code != STILL_ACTIVE) {
                const DWORD cap = m_capture_pid;
                const DWORD launch = m_launch_pid;
                const bool capturing_child =
                    (cap != 0 && cap != launch && process_ops().is_running(cap));
                const bool still_in_rebind_grace =
                    rpc_unix_epoch_ms() <= m_pid_rebind_deadline_unix_ms + 3000;
                if (capturing_child || still_in_rebind_grace) {
                } else {
                    notify_remote_exit_if_needed("launch_process_exited");
                    break;
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

        if (!m_process_session || !m_capture_source) {
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
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        m_main_window = primary_hwnd_from_surfaces(surfaces);//选主窗口,以最大面积者优先
        apply_launch_window_placement(m_main_window);//可能执行一次窗口摆放

        // Capture and compose.
        const auto t_cap_begin = std::chrono::steady_clock::now();
        CaptureGrabOutcome outcome = ProcessUiCapture::grab_process_ui_rgb(m_capture_pid, surfaces, m_ui_capture_options, *m_capture_source, now_unix_ms);
        const auto t_after_cap = std::chrono::steady_clock::now();

        m_last_capture_used_hw = outcome.used_hw_capture;

        if (!outcome.ok || outcome.frame.empty() || outcome.width <= 0 || outcome.height <= 0) {
            // No new frame; pacing continues.
        } else {
            // Sanitize in capture thread; do not treat drops as session health failures.
            const bool keep = FrameSanitizer::sanitize_frame(outcome.frame, outcome.width, outcome.height, had_successful_video, !last_good_rgb.empty(), last_good_rgb, last_good_w, last_good_h);

            if (keep) {
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
                cf.unix_ms = now_unix_ms;
                cf.t_cap_begin = t_cap_begin;
                cf.t_cap_done = t_after_cap;
                cf.width = outcome.width;
                cf.height = outcome.height;
                cf.cap_min_left = outcome.cap_min_left;
                cf.cap_min_top = outcome.cap_min_top;
                cf.used_hw_capture = outcome.used_hw_capture;
                cf.rgb = std::move(outcome.frame);

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
            std::unique_lock<std::mutex> lk(m_latest_frame.mtx);
            m_latest_frame.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !m_threads_running.load(std::memory_order_acquire) ||
                       (m_latest_frame.latest.has_value() && m_latest_frame.latest->frame_id != last_encoded_id);
            });
            if (!m_threads_running.load(std::memory_order_acquire)) break;
            if (!m_latest_frame.latest.has_value()) continue;
            if (m_latest_frame.latest->frame_id == last_encoded_id) continue;

            // Take a snapshot of the latest (copy/move rgb to avoid holding lock during encode).
            frame = std::move(m_latest_frame.latest.value());
            // Keep the slot populated with an empty placeholder so overwrite count remains meaningful.
            m_latest_frame.latest.reset();
        }

        if (!frame.has_value() || !m_video_encode_pipeline) continue;

        CapturedFrame& cf = frame.value();
        last_encoded_id = cf.frame_id;

        int target_w = cf.width;
        int target_h = cf.height;
        bool applied_layout = false;
        const bool layout_ok = m_video_encode_pipeline->ensure_encoder_layout(
            cf.width, cf.height, m_had_successful_video.load(std::memory_order_relaxed), target_w, target_h, applied_layout);
        if (!layout_ok) {
            continue;
        }

        VideoEncodeResult enc = m_video_encode_pipeline->encode_frame(
            cf.rgb,
            cf.width,
            cf.height,
            applied_layout,
            cf.t_cap_begin,
            cf.t_cap_done);

        if (enc.encode_ok && !enc.sample.empty() && !enc.invalid_payload) {
            EncodedSample es;
            es.frame_id = cf.frame_id;
            es.unix_ms = enc.frame_unix_ms ? enc.frame_unix_ms : cf.unix_ms;
            es.capture_ms = enc.capture_ms;
            es.encode_ms = enc.encode_ms;
            es.sample = std::move(enc.sample);
            es.used_hw_capture = cf.used_hw_capture;
            es.w = target_w;
            es.h = target_h;

            {
                std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
                while (m_latest_encoded.q.size() >= m_latest_encoded.capacity) {
                    m_latest_encoded.q.pop_front();
                    m_latest_encoded.dropped_by_overflow++;
                }
                m_latest_encoded.q.push_back(es);
                m_latest_encoded.pushed++;
            }

            {
                std::lock_guard<std::mutex> lk(m_last_enc_mtx);
                m_last_capture_ms = es.capture_ms;
                m_last_encode_ms = es.encode_ms;
                m_last_frame_unix_ms = es.unix_ms;
                m_last_used_hw_capture = es.used_hw_capture;
                m_last_capture_w = es.w;
                m_last_capture_h = es.h;
            }

            // Update last good sample for steady-hold mode.
            {
                std::lock_guard<std::mutex> lk(m_last_good_sample_mtx);
                m_last_good_video_sample = es.sample;
            }
            m_have_last_good_sample.store(true, std::memory_order_relaxed);
            m_had_successful_video.store(true, std::memory_order_relaxed);
        } else {
            // If encode fails but we already have a good sample, request keyframe to recover quickly.
            if (m_have_last_good_sample.load(std::memory_order_relaxed)) {
                request_force_keyframe();
            }
        }
    }
}

//MediaSession 调度线程，样本消费者,每个视频 tick 来“取一次样本”，把样本交给 sender（外部调用它的人会把 out_sample 传给 m_sender->on_video_sample(...)）。
void remote_video_engine::produce_next_video_sample(rtc::binary& out_sample, remote_capture_telemetry& out_telemetry)
{
    out_sample.clear();
    out_telemetry = remote_capture_telemetry{};

    const uint64_t now_unix_ms = rpc_unix_epoch_ms();
    out_telemetry.last_frame_unix_ms = now_unix_ms;
    fill_capture_backend_telemetry(out_telemetry);

    // Pop latest encoded sample (no blocking). If none, apply steady-hold fallback policy.
    EncodedSample es;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
        if (!m_latest_encoded.q.empty()) {
            es = std::move(m_latest_encoded.q.back());
            m_latest_encoded.q.clear();
            have = true;
        }
    }

    if (have && !es.sample.empty()) {
        out_sample = std::move(es.sample);
        out_telemetry.last_capture_ms = es.capture_ms;
        out_telemetry.last_encode_ms = es.encode_ms;
        out_telemetry.last_frame_unix_ms = es.unix_ms ? es.unix_ms : now_unix_ms;
        out_telemetry.capture_width = es.w;
        out_telemetry.capture_height = es.h;

        static auto s_last_diag = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last_diag >= std::chrono::seconds(1)) {
            s_last_diag = now;
            uint64_t dropped_overwrite = 0;
            uint64_t dropped_overflow = 0;
            uint64_t stored = 0;
            uint64_t pushed = 0;
            size_t sendq_cap = 0;
            {
                std::lock_guard<std::mutex> lk(m_latest_frame.mtx);
                dropped_overwrite = m_latest_frame.dropped_by_overwrite;
                stored = m_latest_frame.stored_frames;
            }
            {
                std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
                dropped_overflow = m_latest_encoded.dropped_by_overflow;
                pushed = m_latest_encoded.pushed;
                sendq_cap = m_latest_encoded.capacity;
            }
            std::cout << "[latency][agent_q] stored=" << stored
                      << " pushed=" << pushed
                      << " drop_overwrite=" << dropped_overwrite
                      << " drop_sendq=" << dropped_overflow
                      << " sendq_cap=" << sendq_cap
                      << std::endl;
        }
        return;
    }

    // No fresh sample; reuse last good sample if configured.
    apply_emit_fail_policy(out_sample, false);
    {
        std::lock_guard<std::mutex> lk(m_last_enc_mtx);
        out_telemetry.last_capture_ms = m_last_capture_ms;
        out_telemetry.last_encode_ms = m_last_encode_ms;
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
    if (!m_process_session) return;

    if (m_main_window && m_process_session->is_window_viable_for_capture(m_main_window)) return;
    m_main_window = nullptr;

    if (m_target_exe_base_name.empty()) return;

    m_main_window = SessionHealthPolicy::try_recover_main_window(
        *m_process_session,
        m_launch_pid,
        m_capture_pid,
        m_target_exe_base_name,
        m_allow_pid_rebind_by_exename,
        m_had_successful_video.load(std::memory_order_relaxed),
        m_pid_rebind_deadline_unix_ms);

    apply_launch_window_placement(m_main_window);
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

void remote_video_engine::fill_capture_backend_telemetry(remote_capture_telemetry& out_telemetry) const
{
    out_telemetry.last_capture_used_hw = m_last_capture_used_hw;
    out_telemetry.dxgi_disabled_for_session = false;
    out_telemetry.top_black_strip_streak = 0;
    out_telemetry.dxgi_instability_score = 0;
    out_telemetry.force_software_capture_until_unix_ms = 0;
}
