#include "session/remote_video_engine.h"

#include "app/runtime_config.h"
#include "capture/capture_source_factory.h"
#include "capture/process_ui_capture.h"
#include "common/rpc_time.h"
#include "common/window_ops.h"
#include "common/character_conversion.h"
#include "encode/video_encode_pipeline.h"
#include "input/input_controller.h"
#include "common/process_ops.h"
#include "session/session_health_policy.h"
#include "session/capture_target_resolver.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

namespace {

static void release_raw_frame_owned(rpc_video_contract::RawFrame& f)
{
    if (f.owned.release && f.owned.opaque) {
        f.owned.release(f.owned.opaque);
    }
    f.owned = {};
    f.plane_count = 0;
    for (auto& p : f.planes) p = {};
    if (f.gpu.release && f.gpu.opaque) {
        f.gpu.release(f.gpu.opaque);
    }
    f.gpu = {};
    f.ext = nullptr;
}

static BmpDumpDiag make_bmp_dump_diag_from_hw(bool used_hw)
{
    BmpDumpDiag d;
    d.use_hw_capture = used_hw;
    d.force_software_active = false;
    d.top_black_strip_streak = 0;
    d.dxgi_instability_score = 0;
    d.dxgi_disabled_for_session = false;
    return d;
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
        m_capture_source.reset();
    } else {
        m_capture_kind = resolved.kind;
        m_capture_source = create_capture_source(m_capture_kind);
        if (m_capture_source) {
            m_capture_source->init();
        }
    }
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
        if (m_process_ops) {
            // Keep liveness checks consistent with capture target resolution (PID drift / rebind).
            const auto resolved = CaptureTargetResolver::resolve(
                *m_process_ops,
                m_launch_pid,
                m_capture_pid,
                m_main_window,
                m_process_ops->target_exe_base_name_lower(),
                m_had_successful_video.load(std::memory_order_relaxed),
                m_pid_rebind_deadline_unix_ms);

            m_main_window = resolved.main_hwnd ? resolved.main_hwnd : m_main_window;

            const DWORD owner_pid = resolved.main_hwnd_owner_pid;
            const DWORD cap_pid = resolved.capture_pid;

            // 1) Prefer main window owner PID when available.
            if (owner_pid != 0) {
                auto h = m_process_ops->open_process(owner_pid, PROCESS_QUERY_LIMITED_INFORMATION);
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
            } else if (cap_pid != 0) {
                // 2) Fallback: if we have a capture PID but no main window, check it is still running.
                if (!m_process_ops->is_running(cap_pid)) {
                    // Preserve grace behavior for PID drift: allow a short rebind window before declaring exit.
                    const bool still_in_rebind_grace = rpc_unix_epoch_ms() <= m_pid_rebind_deadline_unix_ms + 3000;
                    if (!still_in_rebind_grace) {
                        notify_remote_exit_if_needed("capture_pid_exited");
                        break;
                    }
                }
            } else if (m_pi.hProcess) {
                // 3) Last fallback: launch handle exit (legacy behavior).
                DWORD exit_code = 0;
                const bool ok = m_process_ops->get_exit_code(m_pi.hProcess, exit_code);
                if (ok && exit_code != STILL_ACTIVE) {
                    const DWORD cap = m_capture_pid;
                    const DWORD launch = m_launch_pid;
                    const bool capturing_child = (cap != 0 && cap != launch && m_process_ops->is_running(cap));
                    const bool still_in_rebind_grace = rpc_unix_epoch_ms() <= m_pid_rebind_deadline_unix_ms + 3000;
                    if (!(capturing_child || still_in_rebind_grace)) {
                        notify_remote_exit_if_needed("launch_process_exited");
                        break;
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}


//采集生产者,不断抓取进程 UI 画面（DXGI/GDI）并合成 RGB 帧，写入最新帧槽位。
void remote_video_engine::capture_loop()
{
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

        //1. Resolve capture PID + surfaces (handles PID drift / rebind).
        const auto resolved = CaptureTargetResolver::resolve(*m_process_ops, m_launch_pid, m_capture_pid, m_main_window, m_process_ops ? m_process_ops->target_exe_base_name_lower() : std::string{}, m_had_successful_video.load(std::memory_order_relaxed),m_pid_rebind_deadline_unix_ms);
        const std::vector<window_ops::window_info> surfaces = resolved.surfaces;

        if (surfaces.empty()) {
            m_main_window = nullptr;
            //1.1 Resolver already attempted recovery and may have rebound m_capture_pid.
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
                      << " resolver_why=" << (resolved.why ? resolved.why : "")
                      << " prev_pid=" << resolved.previous_capture_pid
                      << " owner_pid=" << resolved.main_hwnd_owner_pid
                      << " pid_rebound=" << (resolved.capture_pid_rebound ? 1 : 0)
                      << " main_from_surfaces=" << (resolved.main_hwnd_selected_from_surfaces ? 1 : 0)
                      << " notify_remote_exit_if_needed=" << notify
				<< std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        m_main_window = resolved.main_hwnd; // 主窗口选择策略统一由 resolver 负责

        //2. Capture and compose.
        const uint64_t prep_unix_ms = rpc_unix_epoch_ms();
        const uint64_t frame_id = m_frame_id_seq.fetch_add(1, std::memory_order_relaxed) + 1;
        rpc_video_contract::RawFrame rf;
        rpc_video_contract::TelemetrySnapshot telem;
        const bool ok = ProcessUiCapture::grab_process_ui_raw_frame(m_capture_pid, surfaces, m_ui_capture_options, *m_capture_source, now_unix_ms, prep_unix_ms, frame_id, rf, telem);

        if (!ok) {
            // No new frame; pacing continues.
        } else {
            auto* vec = static_cast<std::vector<uint8_t>*>(rf.owned.opaque);
            const int w = rf.coded_size.w;
            const int h = rf.coded_size.h;
            
            // Sanitize in capture thread; do not treat drops as session health failures.
            input_controller::instance()->set_capture_screen_rect(rf.visible_rect.x, rf.visible_rect.y, w, h);

            const BmpDumpDiag bmp_diag = make_bmp_dump_diag_from_hw(telem.backend == rpc_video_contract::CaptureBackend::Dxgi);
            m_bmp_dump.dump_capture_if_needed(*vec, w, h, bmp_diag);

            CapturedRawFrameWithTelemetry pkt;
            pkt.frame = std::move(rf);
            pkt.telem = telem;

            {
                std::lock_guard<std::mutex> lk(m_latest_frame.mtx);
                if (m_latest_frame.latest.has_value()) {
                    release_raw_frame_owned(m_latest_frame.latest->frame);
                }
                m_latest_frame.latest = std::move(pkt);
            }
            m_latest_frame.cv.notify_one();
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
        std::optional<CapturedRawFrameWithTelemetry> packet;
        {
            //1.等待新帧（最多 50ms）
            std::unique_lock<std::mutex> lk(m_latest_frame.mtx);
            m_latest_frame.cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return !m_threads_running.load(std::memory_order_acquire) ||
                       (m_latest_frame.latest.has_value() && m_latest_frame.latest->frame.frame_id != last_encoded_id);
            });
            if (!m_threads_running.load(std::memory_order_acquire)) break;
            if (!m_latest_frame.latest.has_value()) continue;
            if (m_latest_frame.latest->frame.frame_id == last_encoded_id) continue;
            
            //2.“拿走最新帧”并立即释放锁Take a snapshot of the latest (copy/move rgb to avoid holding lock during encode).
            packet = std::move(m_latest_frame.latest.value());
            // Keep the slot populated with an empty placeholder so overwrite count remains meaningful.
            m_latest_frame.latest.reset();
        }

        //3.确认编码管线存在
        if (!packet.has_value() || !m_video_encode_pipeline) {
            if (packet.has_value()) release_raw_frame_owned(packet->frame);
            continue;
        }

        { // backlog guard
            std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
            if (m_latest_encoded.q.size() > 3) {
                release_raw_frame_owned(packet->frame);
                continue;
            }
        }
        CapturedRawFrameWithTelemetry& pkt = packet.value();
        rpc_video_contract::RawFrame& rf = pkt.frame;
        last_encoded_id = rf.frame_id;

        const int captured_w = rf.coded_size.w;
        const int captured_h = rf.coded_size.h;
        int target_w = captured_w;
        int target_h = captured_h;
        bool applied_layout = false;
        //4.确保编码器布局/分辨率策略一致
        const bool layout_ok = m_video_encode_pipeline->ensure_encoder_layout(captured_w, captured_h, m_had_successful_video.load(std::memory_order_relaxed), target_w, target_h, applied_layout);
        if (!layout_ok) {
            release_raw_frame_owned(rf);
            continue;
        }

        //5.编码
        auto* vec = static_cast<std::vector<uint8_t>*>(rf.owned.opaque);
        if (!vec) {
            release_raw_frame_owned(rf);
            continue;
        }
        VideoEncodeResult enc = m_video_encode_pipeline->encode_frame(*vec, captured_w, captured_h, applied_layout);

        if (enc.encode_ok && !enc.sample.empty() && !enc.invalid_payload) {
            EncodedFrameWithTelemetry out;
            out.payload_storage = std::move(enc.sample);

            out.telem = pkt.telem;
            out.telem.capture_size = rpc_video_contract::VideoSize{ target_w, target_h };
            out.telem.encode_unix_ms = enc.frame_unix_ms ? enc.frame_unix_ms : rpc_unix_epoch_ms();

            {
                //5.1 推入最新编码队列
                std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
                m_latest_encoded.q.push_back(std::move(out));
            }

            m_had_successful_video.store(true, std::memory_order_relaxed);
        } else {
            // If encode fails but we already have a good sample, request keyframe to recover quickly.
            request_force_keyframe();
        }

        // RawFrame owns heap buffer; always release after encoding attempt.
        release_raw_frame_owned(rf);
    }
}

//MediaSession 调度线程，样本消费者,每个视频 tick 来“取一次样本”，把样本交给 sender（外部调用它的人会把 out_sample 传给 m_sender->on_video_sample(...)）。
void remote_video_engine::produce_next_video_sample(rtc::binary& out_sample, rpc_video_contract::TelemetrySnapshot& out_telem)
{
    out_sample.clear();
    out_telem = rpc_video_contract::TelemetrySnapshot{};

    //1.无阻塞地“取走最新编码样本”Pop latest encoded sample (no blocking). If none, apply steady-hold fallback policy.
    EncodedFrameWithTelemetry es;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(m_latest_encoded.mtx);
        if (!m_latest_encoded.q.empty()) {
            es = std::move(m_latest_encoded.q.front());
			m_latest_encoded.q.pop_front();
            have = true;
        }
    }

    //2.若拿到了有效样本：填充输出并返回
    if (have && !es.payload_storage.empty()) {
        out_sample = std::move(es.payload_storage);
        out_telem = es.telem;
        return;
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
    m_had_successful_video.store(false, std::memory_order_relaxed);

    m_window_missing_since_unix_ms = 0;

    if (m_video_encode_pipeline) m_video_encode_pipeline->reset_for_stream_start();
    if (m_capture_source) m_capture_source->reset_session_recovery();
    m_bmp_dump.reset_session();
}

bool remote_video_engine::launch_attached_remote_process()
{
    if (!m_process_ops) return false;
    if (!m_process_ops->start()) return false;

    m_launch_pid = m_process_ops->launch_pid();
    m_capture_pid = m_process_ops->capture_pid();
    m_process_ops->detach_launch_process_info(m_pi);

    // 不要因等待窗口而阻塞流创建：但要允许 PID 漂移重绑（如 notepad）。
    auto res = CaptureTargetResolver::resolve(*m_process_ops, m_launch_pid, m_capture_pid, nullptr, m_process_ops->target_exe_base_name_lower(),false, rpc_unix_epoch_ms() + 5000);
    m_main_window = res.main_hwnd;
    return true;
}