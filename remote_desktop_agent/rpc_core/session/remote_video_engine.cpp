#include "session/remote_video_engine.h"

#include "session/remote_process_session.h"

#include "app/runtime_config.h"
#include "capture/capture_backend_state.h"
#include "capture/capture_discard_policy.h"
#include "capture/capture_grab_outcome.h"
#include "capture/dxgi_capture.h"
#include "capture/frame_sanitizer.h"
#include "capture/gdi_capture.h"
#include "capture/process_surface_enumerator.h"
#include "capture/process_ui_capture.h"
#include "common/rpc_time.h"
#include "encode/video_encode_pipeline.h"
#include "input/input_controller.h"
#include "session/process_lifecycle.h"
#include "session/session_health_policy.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace {

BmpDumpDiag make_bmp_dump_diag(const CaptureGrabOutcome& o, const CaptureBackendState* st, uint64_t now_unix_ms)
{
    (void)now_unix_ms;
    BmpDumpDiag d;
    d.use_hw_capture = o.used_hw_capture;
    d.force_software_active = false;
    if (st) {
        d.top_black_strip_streak = st->get_top_black_strip_streak();
        d.dxgi_instability_score = st->get_dxgi_instability_score();
        d.dxgi_disabled_for_session = st->is_dxgi_disabled_for_session();
    }
    return d;
}

HWND primary_hwnd_from_surfaces(const std::vector<ProcessSurfaceInfo>& surfaces)
{
    if (surfaces.empty()) return nullptr;
    HWND best = nullptr;
    int best_area = -1;
    for (const auto& s : surfaces) {
        const int w = s.rect_screen.right - s.rect_screen.left;
        const int h = s.rect_screen.bottom - s.rect_screen.top;
        const int a = w * h;
        if (a > best_area) {
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

    m_gdi_capture = std::make_unique<GdiCapture>();
    m_dxgi_capture = std::make_unique<DXGICapture>();
    m_capture_backend_state = std::make_unique<CaptureBackendState>();
    m_video_encode_pipeline = std::make_unique<VideoEncodePipeline>();
    m_bmp_dump.configure_from_config();

    m_window_missing_exit_grace_ms = 5000;
    m_video_fps = 30;
    m_video_encode_pipeline->configure(m_video_fps, m_encoder_layout_change_threshold_px, m_encoder_layout_change_required_streak);

    m_ui_capture_options = ProcessUiCapture::load_layout_options_from_config();
}

bool remote_video_engine::is_remote_process_still_running() const
{
    // Prefer the original launch handle when available.
    if (m_pi.hProcess) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(m_pi.hProcess, &exit_code) && exit_code == STILL_ACTIVE) {
            return true;
        }
    }
    const DWORD cap = m_capture_pid;
    const DWORD launch = m_launch_pid;
    if (cap != 0 && process_lifecycle::process_is_running(cap)) return true;
    if (launch != 0 && process_lifecycle::process_is_running(launch)) return true;
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

    const std::string backend =
        ProcessUiCapture::to_lower_ascii(runtime_config::get_string("RPC_CAPTURE_BACKEND", "auto"));
    m_session_uses_dxgi = false;
    if (backend == "dxgi") {
        m_capture_backend_is_auto = false;
        if (!m_dxgi_capture || !m_dxgi_capture->is_available()) {
            std::cout << "[capture] RPC_CAPTURE_BACKEND=dxgi but DXGI is unavailable, terminating remote process\n";
            try {
                if (m_process_session && m_pi.hProcess) {
                    m_process_session->terminate_processes(m_pi, m_capture_pid, m_launch_pid);
                }
            } catch (...) {
            }
            m_pi = {};
            m_capture_pid = 0;
            m_launch_pid = 0;
            m_running = false;
            return;
        }
        m_session_uses_dxgi = true;
    } else if (backend == "gdi") {
        m_capture_backend_is_auto = false;
        m_session_uses_dxgi = false;
    } else {
        m_capture_backend_is_auto = true;
        if (backend != "auto" && !backend.empty()) {
            std::cout << "[capture] unknown RPC_CAPTURE_BACKEND=" << backend << ", using auto\n";
        }
        m_session_uses_dxgi = m_dxgi_capture && m_dxgi_capture->is_available();
    }

    m_capture_backend_state->configure(m_session_uses_dxgi, m_capture_degrade_ms, m_capture_backend_is_auto);
    m_steady_frame_hold = !m_session_uses_dxgi;

    std::cout << "[capture] RPC_CAPTURE_BACKEND resolved session_dxgi=" << (m_session_uses_dxgi ? 1 : 0)
              << " layout=" << static_cast<int>(m_ui_capture_options.composite_layout) << "\n";

    m_exit_notified = false;
    m_exit_watch_thread = std::thread(&remote_video_engine::exit_watch_loop, this);
}

void remote_video_engine::stop()
{
    m_running = false;

    if (m_exit_watch_thread.joinable()) {
        m_exit_watch_thread.join();
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
        if (mw && IsWindow(mw)) {
            GetWindowThreadProcessId(mw, &window_owner_pid);
        }

        if (window_owner_pid != 0) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, window_owner_pid);
            if (!h) {
                notify_remote_exit_if_needed("main_window_owner_open_failed");
                break;
            }
            DWORD exit_code = 0;
            GetExitCodeProcess(h, &exit_code);
            CloseHandle(h);
            if (exit_code != STILL_ACTIVE) {
                notify_remote_exit_if_needed("main_window_owner_exited");
                break;
            }
        } else if (m_pi.hProcess) {
            DWORD exit_code = 0;
            const BOOL ok = GetExitCodeProcess(m_pi.hProcess, &exit_code);
            if (ok && exit_code != STILL_ACTIVE) {
                const DWORD cap = m_capture_pid;
                const DWORD launch = m_launch_pid;
                const bool capturing_child =
                    (cap != 0 && cap != launch && process_lifecycle::process_is_running(cap));
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
    if (m_steady_frame_hold && m_have_last_good_sample) {
        out_sample = m_last_good_video_sample;
    } else {
        out_sample.clear();
    }
    if (request_idr && m_have_last_good_sample) {
        request_force_keyframe();
    }
}

void remote_video_engine::produce_next_video_sample(rtc::binary& out_sample, remote_capture_telemetry& out_telemetry)
{
    out_sample.clear();
    out_telemetry = remote_capture_telemetry{};

    const uint64_t now_unix_ms = rpc_unix_epoch_ms();
    out_telemetry.last_frame_unix_ms = now_unix_ms;
    fill_capture_backend_telemetry(out_telemetry);

    if (!m_process_session || !m_video_encode_pipeline || !m_capture_backend_state) {
        return;
    }

    const std::vector<ProcessSurfaceInfo> surfaces = ProcessSurfaceEnumerator::enumerate_visible_top_level(m_capture_pid);

    if (surfaces.empty()) {
        m_main_window = nullptr;
        try_recover_main_window(now_unix_ms);
        out_sample.clear();
        const bool notify = SessionHealthPolicy::should_notify_remote_exit(
            m_had_successful_video, now_unix_ms, m_window_missing_since_unix_ms, m_window_missing_exit_grace_ms);
        if (notify) {
            // Window can be temporarily missing during app startup/transition.
            // Only treat it as remote exit when the process is actually gone.
            if (is_remote_process_still_running()) {
                notify_window_missing_if_needed("no_surfaces_grace_expired_but_process_alive", now_unix_ms);
            } else {
                notify_remote_exit_if_needed("no_surfaces_grace_expired");
                m_running = false;
            }
        }
        return;
    }

    m_main_window = primary_hwnd_from_surfaces(surfaces);

    ProcessUiCaptureOptions opts = m_ui_capture_options;
    if (!m_capture_backend_is_auto) {
        opts.session_backend = m_session_uses_dxgi ? ProcessUiSessionBackendMode::Dxgi : ProcessUiSessionBackendMode::Gdi;
    } else if (m_session_uses_dxgi && m_capture_backend_state->is_dxgi_disabled_for_session()) {
        opts.session_backend = ProcessUiSessionBackendMode::Gdi;
    } else {
        opts.session_backend =
            m_session_uses_dxgi ? ProcessUiSessionBackendMode::Dxgi : ProcessUiSessionBackendMode::Gdi;
    }

    m_capture_backend_state->set_last_capture_used_hw(opts.session_backend == ProcessUiSessionBackendMode::Dxgi);

    const auto t_cap_begin = std::chrono::steady_clock::now();
    CaptureGrabOutcome outcome = ProcessUiCapture::grab_process_ui_rgb(
        m_capture_pid,
        opts,
        *m_gdi_capture,
        *m_dxgi_capture,
        *m_capture_backend_state,
        now_unix_ms);
    const auto t_after_cap = std::chrono::steady_clock::now();

    int capture_ms = 0;
    const bool discard_slow = CaptureDiscardPolicy::should_discard_if_capture_too_slow(
        m_had_successful_video,
        m_have_last_good_sample,
        t_cap_begin,
        t_after_cap,
        capture_ms);

    capture_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t_after_cap - t_cap_begin).count());
    out_telemetry.last_capture_ms = static_cast<uint32_t>(capture_ms);

    if (!outcome.ok && outcome.need_hold_on_empty_fallback && m_had_successful_video && m_have_last_good_sample) {
        out_sample = m_last_good_video_sample;
        request_force_keyframe();
        return;
    }

    const bool discard_empty = CaptureDiscardPolicy::should_discard_if_empty_frame(outcome.frame, outcome.width, outcome.height);

    if (discard_slow) {
        out_sample.clear();
        return;
    }

    if (discard_empty || !outcome.ok) {
        apply_emit_fail_policy(out_sample, true);
        return;
    }

    if (outcome.used_hw_capture && static_cast<uint64_t>(capture_ms) >= m_capture_degrade_ms) {
        m_capture_backend_state->on_slow_capture(now_unix_ms);
    }

    const bool keep = FrameSanitizer::sanitize_frame(
        outcome.frame,
        outcome.width,
        outcome.height,
        outcome.cap_min_left,
        outcome.cap_min_top,
        m_had_successful_video,
        m_have_last_good_sample,
        m_session_uses_dxgi,
        m_last_good_rgb_frame,
        m_last_good_rgb_w,
        m_last_good_rgb_h,
        *m_gdi_capture,
        *m_capture_backend_state);
    if (!keep) {
        apply_emit_fail_policy(out_sample, true);
        return;
    }

    input_controller::instance()->set_capture_screen_rect(
        outcome.cap_min_left, outcome.cap_min_top, outcome.width, outcome.height);

    const BmpDumpDiag bmp_diag = make_bmp_dump_diag(outcome, m_capture_backend_state.get(), now_unix_ms);
    m_bmp_dump.dump_capture_if_needed(outcome.frame, outcome.width, outcome.height, bmp_diag);

    int target_w = outcome.width;
    int target_h = outcome.height;
    bool applied_layout = false;

    const bool layout_ok = m_video_encode_pipeline->ensure_encoder_layout(
        outcome.width, outcome.height, m_had_successful_video, target_w, target_h, applied_layout);
    out_telemetry.capture_width = target_w;
    out_telemetry.capture_height = target_h;

    if (!layout_ok) {
        apply_emit_fail_policy(out_sample, true);
        return;
    }

    m_bmp_dump.dump_encode_if_needed(outcome.frame, outcome.width, outcome.height, bmp_diag);

    VideoEncodeResult enc = m_video_encode_pipeline->encode_frame(
        outcome.frame,
        outcome.width,
        outcome.height,
        applied_layout,
        t_cap_begin,
        t_after_cap);

    out_telemetry.last_encode_ms = enc.encode_ok ? enc.encode_ms : 0;
    out_telemetry.last_capture_ms = enc.encode_ok ? enc.capture_ms : out_telemetry.last_capture_ms;
    out_telemetry.last_frame_unix_ms = enc.frame_unix_ms ? enc.frame_unix_ms : now_unix_ms;

    if (enc.encode_ok && !enc.sample.empty() && !enc.invalid_payload) {
        out_sample = std::move(enc.sample);

        m_last_good_video_sample = out_sample;
        m_have_last_good_sample = true;
        m_had_successful_video = true;

        m_last_good_rgb_frame = std::move(outcome.frame);
        m_last_good_rgb_w = outcome.width;
        m_last_good_rgb_h = outcome.height;
        return;
    }

    if (m_have_last_good_sample) {
        out_sample = m_last_good_video_sample;
        request_force_keyframe();
    } else {
        out_sample.clear();
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
    m_had_successful_video = false;
    m_last_good_video_sample.clear();
    m_have_last_good_sample = false;

    m_last_good_rgb_frame.clear();
    m_last_good_rgb_w = 0;
    m_last_good_rgb_h = 0;

    m_window_missing_since_unix_ms = 0;

    if (m_video_encode_pipeline) m_video_encode_pipeline->reset_for_stream_start();
    if (m_capture_backend_state) m_capture_backend_state->reset_for_stream_start();
    m_bmp_dump.reset_session();
}

void remote_video_engine::try_recover_main_window(uint64_t now_unix_ms)
{
    if (!m_process_session) return;
    (void)now_unix_ms;

    if (m_main_window && m_process_session->is_window_viable_for_capture(m_main_window)) return;
    m_main_window = nullptr;

    if (m_target_exe_base_name.empty()) return;

    m_main_window = SessionHealthPolicy::try_recover_main_window(
        *m_process_session,
        m_launch_pid,
        m_capture_pid,
        m_target_exe_base_name,
        m_allow_pid_rebind_by_exename,
        m_had_successful_video,
        m_pid_rebind_deadline_unix_ms);
}

void remote_video_engine::fill_capture_backend_telemetry(remote_capture_telemetry& out_telemetry) const
{
    if (!m_capture_backend_state) return;
    out_telemetry.last_capture_used_hw = m_capture_backend_state->get_last_capture_used_hw();
    out_telemetry.dxgi_disabled_for_session = m_capture_backend_state->is_dxgi_disabled_for_session();
    out_telemetry.top_black_strip_streak = m_capture_backend_state->get_top_black_strip_streak();
    out_telemetry.dxgi_instability_score = m_capture_backend_state->get_dxgi_instability_score();
    out_telemetry.force_software_capture_until_unix_ms = 0;
}
