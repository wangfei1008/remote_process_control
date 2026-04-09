#include "session/remote_video_engine.h"

#include "session/remote_process_session.h"

#include "capture/capture_backend_state.h"
#include "capture/capture_coordinator.h"
#include "capture/capture_discard_policy.h"
#include "capture/dxgi_capture.h"
#include "capture/frame_sanitizer.h"
#include "capture/gdi_capture.h"
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
    BmpDumpDiag d;
    d.use_hw_capture = o.used_hw_capture;
    if (st) {
        const uint64_t until = st->get_force_software_capture_until_unix_ms();
        d.force_software_active = (until != 0 && now_unix_ms < until);
        d.top_black_strip_streak = st->get_top_black_strip_streak();
        d.dxgi_instability_score = st->get_dxgi_instability_score();
        d.dxgi_disabled_for_session = st->is_dxgi_disabled_for_session();
    }
    return d;
}

} // namespace

remote_video_engine::remote_video_engine(std::string exe_path, std::function<void()> on_remote_process_exit)
    : m_exe_path(std::move(exe_path))
    , m_on_remote_process_exit(std::move(on_remote_process_exit))
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

    // 启动被控进程 + 窗口选择
    std::string target_base;
    m_pi = {};
    m_launch_pid = 0;
    m_capture_pid = 0;
    m_main_window = nullptr;
    if (!m_process_session->launch_process(
            m_exe_path, m_pi, m_launch_pid, m_capture_pid, m_main_window, target_base)) {
        m_running = false;
        return;
    }
    m_target_exe_base_name = target_base;
    m_pid_rebind_deadline_unix_ms = rpc_unix_epoch_ms() + 5000;

    // 配置采集后端健康管理
    const bool hw_capture_supported = m_dxgi_capture->is_available();
    const bool hw_capture_active = hw_capture_supported;
    const bool locked_use_hw_capture = hw_capture_supported;
    m_capture_backend_state->configure(
        hw_capture_supported,
        hw_capture_active,
        m_lock_capture_backend,
        locked_use_hw_capture,
        m_capture_degrade_ms);
    m_steady_frame_hold = m_lock_capture_backend && !locked_use_hw_capture;

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
            m_process_session->terminate_processes(m_pi, m_capture_pid, m_launch_pid);
        }
    } catch (...) {
    }
}

void remote_video_engine::notify_remote_exit_if_needed()
{
    if (m_exit_notified.exchange(true)) return;
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
        // 已绑定主窗时：以「窗口所属进程」为准（记事本等可能先起短时启动器 PID，再子进程出窗）
        DWORD window_owner_pid = 0;
        HWND mw = m_main_window;
        if (mw && IsWindow(mw)) {
            GetWindowThreadProcessId(mw, &window_owner_pid);
        }

        if (window_owner_pid != 0) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, window_owner_pid);
            if (!h) {
                notify_remote_exit_if_needed();
                break;
            }
            DWORD exit_code = 0;
            GetExitCodeProcess(h, &exit_code);
            CloseHandle(h);
            if (exit_code != STILL_ACTIVE) {
                notify_remote_exit_if_needed();
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
                    // 启动器已退出但采集可能已切到子进程，或仍在等待首窗绑定
                } else {
                    notify_remote_exit_if_needed();
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

    // 1) 恢复/检查主窗口
    if (m_main_window && !m_process_session->is_window_viable_for_capture(m_main_window)) {
		std::cout << "[health] main window lost, hwnd=" << m_main_window << std::endl;
        m_main_window = nullptr;
    }
    if (!m_main_window) {
        try_recover_main_window(now_unix_ms);
    }

    // 2) 无窗口：与 StreamSource::tick_window 一致，输出空样本推进时间轴（不在 DXGI 缺窗时重复上一帧 H264）。
    if (!m_main_window) {
        out_sample.clear();

        const bool notify = SessionHealthPolicy::should_notify_remote_exit(m_had_successful_video, now_unix_ms, m_window_missing_since_unix_ms, m_window_missing_exit_grace_ms);
        if (notify) {
            notify_remote_exit_if_needed();
            m_running = false;
        }
        return;
    }

    // 3) 决策采集后端（dxgi/gdi）
    const bool use_hw_capture = m_capture_backend_state->decide_use_hw_capture(now_unix_ms);

    // 4) 采集 + 过滤
    const auto t_cap_begin = std::chrono::steady_clock::now();
    CaptureGrabOutcome outcome = CaptureCoordinator::grab_rgb_frame(
        use_hw_capture,
        m_capture_all_windows,
        m_lock_capture_backend,
        m_capture_pid,
        m_main_window,
        *m_gdi_capture,
        *m_dxgi_capture,
        *m_capture_backend_state);
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

    // 与 RemoteProcessStreamSource::grab_rgb_frame：GDI 回退仍空时需要 hold + 请求关键帧。
    if (!outcome.ok && outcome.need_hold_on_empty_fallback && m_had_successful_video && m_have_last_good_sample) {
        out_sample = m_last_good_video_sample;
        request_force_keyframe();
        return;
    }

    const bool discard_empty = CaptureDiscardPolicy::should_discard_if_empty_frame(outcome.frame, outcome.width, outcome.height);

    if (discard_slow) {
        // 与 emit_hold_or_empty_sample(false)：慢采集不重复上一帧码流。
        out_sample.clear();
        return;
    }

    if (discard_empty || !outcome.ok) {
        // 与 emit_hold_or_empty_sample(true)
        apply_emit_fail_policy(out_sample, true);
        return;
    }

    if (use_hw_capture && static_cast<uint64_t>(capture_ms) >= m_capture_degrade_ms) {
        m_capture_backend_state->on_slow_capture(now_unix_ms, use_hw_capture);
    }

    // suspicious frame 判定 +（在允许条件下）顶黑条修复
    const bool keep = FrameSanitizer::sanitize_frame(
        outcome.frame,
        outcome.width,
        outcome.height,
        outcome.cap_min_left,
        outcome.cap_min_top,
        m_had_successful_video,
        m_have_last_good_sample,
        use_hw_capture,
        m_capture_all_windows,
        m_main_window,
        m_last_good_rgb_frame,
        m_last_good_rgb_w,
        m_last_good_rgb_h,
        *m_gdi_capture,
        *m_capture_backend_state);
    if (!keep) {
        apply_emit_fail_policy(out_sample, true);
        return;
    }

    // 与采集帧一致的屏幕矩形（含 DXGI 裁剪、DWM 边框与偶对齐尺寸），供鼠标归一化映射
    input_controller::instance()->set_capture_screen_rect(outcome.cap_min_left, outcome.cap_min_top, outcome.width, outcome.height);

    const BmpDumpDiag bmp_diag = make_bmp_dump_diag(outcome, m_capture_backend_state.get(), now_unix_ms);
    m_bmp_dump.dump_capture_if_needed(outcome.frame, outcome.width, outcome.height, bmp_diag);

    // 5) 编码
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

        // 只用于 frame top strip 修复（尺寸匹配时）。
        m_last_good_rgb_frame = std::move(outcome.frame);
        m_last_good_rgb_w = outcome.width;
        m_last_good_rgb_h = outcome.height;
        return;
    }

    // 编码失败：与 finalize_encode_rgb else 分支一致，重复上一帧并请求 IDR。
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
    out_telemetry.force_software_capture_until_unix_ms = m_capture_backend_state->get_force_software_capture_until_unix_ms();
}

