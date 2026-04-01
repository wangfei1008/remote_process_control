#include "session/remote_process_stream_source.h"
#include "input/input_controller.h"
#include "common/rpc_time.h"
#include "common/runtime_config.h"
#include "common/window_rect_utils.h"
#include "capture/capture_coordinator.h"
#include "capture/capture_discard_policy.h"
#include "capture/frame_sanitizer.h"
#include "session/session_health_policy.h"
#include <chrono>
#include <iostream>
#include <algorithm>

static bool rpc_probe_dxgi_capture_support()
{
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    const bool ok = (dxgi != nullptr && d3d11 != nullptr);
    if (dxgi) FreeLibrary(dxgi);
    if (d3d11) FreeLibrary(d3d11);
    return ok;
}

RemoteProcessStreamSource::RemoteProcessStreamSource() 
    : m_width(1536)
    , m_height(864)
    , m_fps(30)
    , m_active_fps(30)
    , m_idle_fps(5)
    , m_running(false)
{
    ZeroMemory(&m_pi, sizeof(m_pi));
    m_capture_all_windows = runtime_config::get_bool("RPC_CAPTURE_ALL_WINDOWS", false);
    m_idle_fps = (std::max)(1, runtime_config::get_int("RPC_IDLE_FPS", m_idle_fps));
    m_active_fps = (std::max)(1, runtime_config::get_int("RPC_ACTIVE_FPS", m_active_fps));
    if (m_idle_fps > m_active_fps) m_idle_fps = m_active_fps;
    m_fps = m_active_fps;

    m_hw_capture_supported = (rpc_probe_dxgi_capture_support() && m_dxgiCapture.is_available());
    {
        std::string mode = runtime_config::get_string("RPC_CAPTURE_BACKEND", "");
        if (mode.empty()) {
            m_hw_capture_requested = m_hw_capture_supported; // 默认自动模式
            m_lock_capture_backend = false;
        } else {
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mode == "dxgi") {
            m_hw_capture_requested = true;
            m_lock_capture_backend = true;
            m_locked_use_hw_capture = true;
        } else if (mode == "gdi") {
            m_hw_capture_requested = false;
            m_lock_capture_backend = true;
            m_locked_use_hw_capture = false;
        } else {
            m_hw_capture_requested = m_hw_capture_supported; // 自动模式
            m_lock_capture_backend = false;
        }
        }
    }
    m_allow_pid_rebind_by_exename = runtime_config::get_bool("RPC_ALLOW_CAPTURE_PID_REBIND", m_allow_pid_rebind_by_exename);
    m_hw_capture_active = (m_hw_capture_requested && m_hw_capture_supported);
    m_capture_backend_state.configure(
        m_hw_capture_supported,
        m_hw_capture_active,
        m_lock_capture_backend,
        m_locked_use_hw_capture,
        m_capture_degrade_ms);
    std::cout << "[capture] dxgi_supported=" << (m_hw_capture_supported ? 1 : 0)
              << " requested=" << (m_hw_capture_requested ? 1 : 0)
              << " active=" << (m_hw_capture_active ? 1 : 0)
              << " lock=" << (m_lock_capture_backend ? 1 : 0)
              << " locked_backend=" << (m_locked_use_hw_capture ? "dxgi" : "gdi")
              << " mode=" << (m_capture_all_windows ? "all-windows" : "main-window")
              << std::endl;

    m_bmp_dump_writer.configure_from_config();
    m_video_encode_pipeline.configure(m_fps, 8, 5);
}

RemoteProcessStreamSource::~RemoteProcessStreamSource()
{
    terminate();
}

HWND RemoteProcessStreamSource::launch_process(const std::string& exe_path) 
{
    if (!m_remote_process_session.launch_process(exe_path,
                                                 m_pi,
                                                 m_launchPid,
                                                 m_capturePid,
                                                 m_mainWindow,
                                                 m_targetExeBaseName)) {
        return nullptr;
    }
    // 使用窗口矩形（含标题栏/边框等非客户区），
    // 确保 DXGI 与 GDI 采集区域一致。
    RECT winRc{};
    if (window_rect_utils::get_effective_window_rect(m_mainWindow, winRc)) {
        m_width = winRc.right - winRc.left;
        m_height = winRc.bottom - winRc.top;
    }

    // 为 YUV420 编码强制使用偶数分辨率。
    if (m_width > 0 && m_height > 0) {
        m_width = (m_width + 1) & ~1;
        m_height = (m_height + 1) & ~1; // 高度保持偶数
    }
    m_video_encode_pipeline.initialize_encoder(m_width, m_height);
    std::cout << "[proc] launch_process done, hasWindow=" << (m_mainWindow ? 1 : 0) << std::endl;
    return m_mainWindow;
}

void RemoteProcessStreamSource::terminate() 
{
    m_remote_process_session.terminate_processes(m_pi, m_capturePid, m_launchPid);
    m_mainWindow = nullptr;
    m_running = false;
}

void RemoteProcessStreamSource::start()
{
    m_running = true;
    m_had_successful_video = false;
    m_exit_notified = false;
    m_pid_rebind_deadline_unix_ms = rpc_unix_epoch_ms() + 15000; // 仅在启动窗口期允许 PID 重绑定
    m_window_missing_since_unix_ms = 0;
    m_sample_output_state.reset_for_stream_start();
    m_last_good_rgb_frame.clear();
    m_last_good_rgb_w = 0;
    m_last_good_rgb_h = 0;
    m_video_encode_pipeline.reset_for_stream_start();
    m_capture_backend_state.reset_for_stream_start();

    m_bmp_dump_writer.reset_session();
}

void RemoteProcessStreamSource::stop()
{
    // 强生命周期保证：
    // 当上层停止流（前端关闭页面/流）时，
    // 必须终止已拉起进程并释放资源。
    terminate();
}

void RemoteProcessStreamSource::request_force_keyframe()
{
    // 针对丢包/抖动做尽力恢复：让下一编码帧成为 IDR 关键帧。
    m_video_encode_pipeline.request_force_keyframe_with_cooldown(rpc_unix_epoch_ms());
}

void RemoteProcessStreamSource::notify_remote_exit()
{
    if (m_exit_notified)
        return;
    m_exit_notified = true;
    m_running = false;
    std::cout << "[proc] remote application window/process ended, notifying viewers" << std::endl;
    if (m_on_remote_exit) {
        try {
            m_on_remote_exit();
        } catch (...) {
        }
    }
}

bool RemoteProcessStreamSource::tick_window_and_health(std::chrono::steady_clock::time_point& last_no_window_diag)
{
    if (m_had_successful_video && !m_exit_notified) {
        if (m_capturePid != 0) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m_capturePid);
            if (hProc) {
                DWORD code = STILL_ACTIVE;
                if (GetExitCodeProcess(hProc, &code) && code != STILL_ACTIVE) {
                    m_mainWindow = nullptr;
                }
                CloseHandle(hProc);
            }
        }
        if (m_mainWindow && !IsWindow(m_mainWindow)) {
            m_mainWindow = nullptr;
        }
    }

    if (!m_mainWindow) {
        m_mainWindow = SessionHealthPolicy::try_recover_main_window(
            m_remote_process_session,
            m_capturePid,
            m_targetExeBaseName,
            m_allow_pid_rebind_by_exename,
            m_had_successful_video,
            m_pid_rebind_deadline_unix_ms);

        if (!m_mainWindow) {
            SessionHealthPolicy::maybe_log_no_window(
                m_remote_process_session, m_capturePid, last_no_window_diag);
            m_sample_output_state.clear_current();

            const uint64_t nowMs = rpc_unix_epoch_ms();
            if (SessionHealthPolicy::should_notify_remote_exit(
                    m_had_successful_video,
                    nowMs,
                    m_window_missing_since_unix_ms,
                    m_window_missing_exit_grace_ms)) {
                notify_remote_exit();
                return false;
            }
            m_sample_output_state.advance_time(get_sample_duration_us());
            return false;
        }
        m_pid_rebind_deadline_unix_ms = 0;
        m_window_missing_since_unix_ms = 0;
    }
    return true;
}

BmpDumpDiag RemoteProcessStreamSource::make_dump_diag(bool use_hw_capture) const
{
    const uint64_t now_ms = rpc_unix_epoch_ms();
    const uint64_t force_sw_until = m_capture_backend_state.get_force_software_capture_until_unix_ms();
    return BmpDumpDiag{
        use_hw_capture,
        (force_sw_until != 0 && now_ms < force_sw_until),
        m_capture_backend_state.get_top_black_strip_streak(),
        m_capture_backend_state.get_dxgi_instability_score(),
        m_capture_backend_state.is_dxgi_disabled_for_session()
    };
}

void RemoteProcessStreamSource::emit_hold_or_empty_sample(bool request_idr)
{
    const bool steadyFrameHold = (m_lock_capture_backend && !m_locked_use_hw_capture);
    m_sample_output_state.hold_or_empty(steadyFrameHold);
    if (request_idr && m_sample_output_state.has_last_good()) request_force_keyframe();
}

bool RemoteProcessStreamSource::decide_use_hw_capture(uint64_t cap_now_ms)
{
    return m_capture_backend_state.decide_use_hw_capture(cap_now_ms);
}

bool RemoteProcessStreamSource::grab_rgb_frame(int& width, int& height, int& cap_min_left, int& cap_min_top,
                                    std::vector<uint8_t>& frame, bool use_hw_capture, bool& used_hw_capture_out)
{
    CaptureGrabOutcome outcome = CaptureCoordinator::grab_rgb_frame(
        use_hw_capture,
        m_capture_all_windows,
        m_lock_capture_backend,
        m_capturePid,
        m_mainWindow,
        m_gdiCapture,
        m_dxgiCapture,
        m_capture_backend_state);

    frame.swap(outcome.frame);
    used_hw_capture_out = outcome.used_hw_capture;
    width = outcome.width;
    height = outcome.height;
    cap_min_left = outcome.cap_min_left;
    cap_min_top = outcome.cap_min_top;

    if (!outcome.ok && outcome.need_hold_on_empty_fallback && m_had_successful_video &&
        m_sample_output_state.has_last_good()) {
        emit_hold_or_empty_sample(true);
        m_sample_output_state.advance_time(get_sample_duration_us());
        return false;
    }
    return outcome.ok;
}

bool RemoteProcessStreamSource::discard_if_capture_too_slow(std::chrono::steady_clock::time_point t_cap_begin,
                                                 std::chrono::steady_clock::time_point t_after_cap,
                                                 bool use_hw_capture)
{
    int capture_ms = 0;
    if (!CaptureDiscardPolicy::should_discard_if_capture_too_slow(
            m_had_successful_video, m_sample_output_state.has_last_good(), t_cap_begin, t_after_cap, capture_ms)) {
        return false;
    }

    m_capture_backend_state.on_slow_capture(rpc_unix_epoch_ms(), use_hw_capture);
    emit_hold_or_empty_sample(false);
    m_sample_output_state.advance_time(get_sample_duration_us());
    m_last_capture_ms = static_cast<uint32_t>(capture_ms);
    m_last_encode_ms = 0;
    return true;
}

bool RemoteProcessStreamSource::discard_if_empty_frame(const std::vector<uint8_t>& frame, int width, int height)
{
    if (!CaptureDiscardPolicy::should_discard_if_empty_frame(frame, width, height)) return false;
    emit_hold_or_empty_sample(true);
    m_sample_output_state.advance_time(get_sample_duration_us());
    return true;
}

bool RemoteProcessStreamSource::filter_suspicious_frame(std::vector<uint8_t>& frame, int& width, int& height, int& cap_min_left,
                                            int& cap_min_top)
{
    const bool pass = FrameSanitizer::sanitize_frame(
        frame,
        width,
        height,
        cap_min_left,
        cap_min_top,
        m_had_successful_video,
        m_sample_output_state.has_last_good(),
        m_hw_capture_active,
        m_capture_all_windows,
        m_mainWindow,
        m_last_good_rgb_frame,
        m_last_good_rgb_w,
        m_last_good_rgb_h,
        m_gdiCapture,
        m_capture_backend_state);
    if (!pass) {
        static auto lastBadLog = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastBadLog > std::chrono::seconds(1)) {
            lastBadLog = now;
            std::cout << "[capture] suspicious frame detected, discard frame\n";
        }
        emit_hold_or_empty_sample(true);
        m_sample_output_state.advance_time(get_sample_duration_us());
        return false;
    }
    return true;
}

void RemoteProcessStreamSource::ensure_encoder_layout(int captured_w, int captured_h, bool& applied_layout_out)
{
    if (!m_video_encode_pipeline.ensure_encoder_layout(
            captured_w, captured_h, m_had_successful_video, m_width, m_height, applied_layout_out)) {
        applied_layout_out = false;
        return;
    }

    if (applied_layout_out) {
        static auto lastLayoutLog = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLayoutLog > std::chrono::seconds(1)) {
            lastLayoutLog = now;
            std::cout << "[layout] encoder updated: captured=" << captured_w << "x" << captured_h
                      << " -> target=" << m_width << "x" << m_height
                      << std::endl;
        }
    }
}

void RemoteProcessStreamSource::update_fps_pacing_and_mapping(const std::vector<uint8_t>& frame, int captured_w, int captured_h,
                                                   int cap_min_left, int cap_min_top)
{
    InputController::instance()->set_capture_screen_rect(cap_min_left, cap_min_top, captured_w, captured_h);
    m_last_good_rgb_frame = frame;
    m_last_good_rgb_w = captured_w;
    m_last_good_rgb_h = captured_h;
}

void RemoteProcessStreamSource::finalize_encode_rgb(std::vector<uint8_t>& frame, int captured_w, int captured_h,
                                         std::chrono::steady_clock::time_point t_cap_begin,
                                         std::chrono::steady_clock::time_point t_after_cap, bool applied_layout)
{
    VideoEncodeResult result = m_video_encode_pipeline.encode_frame(
        frame,
        captured_w,
        captured_h,
        applied_layout,
        t_cap_begin,
        t_after_cap);

    if (result.encode_ok) {
        m_sample_output_state.set_current(std::move(result.sample));
        m_sample_output_state.advance_time(get_sample_duration_us());
        m_last_capture_ms = result.capture_ms;
        m_last_encode_ms = result.encode_ms;
        m_last_frame_unix_ms = result.frame_unix_ms;
        m_had_successful_video = true;
        m_sample_output_state.commit_current_as_last_good();
    } else {
        if (m_sample_output_state.has_last_good()) {
            m_sample_output_state.set_current_from_last_good_or_clear();
            request_force_keyframe();
        } else {
            m_sample_output_state.clear_current();
        }
        m_sample_output_state.advance_time(get_sample_duration_us());
    }
}

void RemoteProcessStreamSource::load_next_sample()
{
    if (!m_running) return;

    static auto last_no_window_diag = std::chrono::steady_clock::now();
    if (!tick_window_and_health(last_no_window_diag)) return;

    int width = 0, height = 0;
    int cap_min_left = 0, cap_min_top = 0;
    const auto t_cap_begin = std::chrono::steady_clock::now();

    const uint64_t cap_now_ms = rpc_unix_epoch_ms();
    const bool use_hw_capture = decide_use_hw_capture(cap_now_ms);
    bool used_hw_capture_actual = use_hw_capture;

    std::vector<uint8_t> frame;
    if (!grab_rgb_frame(width, height, cap_min_left, cap_min_top, frame, use_hw_capture, used_hw_capture_actual)) return;

    const BmpDumpDiag dump_diag = make_dump_diag(used_hw_capture_actual);
    m_bmp_dump_writer.dump_capture_if_needed(frame, width, height, dump_diag);

    const auto t_after_cap = std::chrono::steady_clock::now();
    if (discard_if_capture_too_slow(t_cap_begin, t_after_cap, use_hw_capture))
        return;
    if (discard_if_empty_frame(frame, width, height))
        return;
    if (!filter_suspicious_frame(frame, width, height, cap_min_left, cap_min_top))
        return;

    m_bmp_dump_writer.dump_encode_if_needed(frame, width, height, dump_diag);

    bool applied_layout = false;
    ensure_encoder_layout(width, height, applied_layout);

    update_fps_pacing_and_mapping(frame, width, height, cap_min_left, cap_min_top);
    finalize_encode_rgb(frame, width, height, t_cap_begin, t_after_cap, applied_layout);
}

rtc::binary RemoteProcessStreamSource::get_sample()
{
    return m_sample_output_state.current_sample();
}

uint64_t RemoteProcessStreamSource::get_sample_time_us()
{
    return m_sample_output_state.sample_time_us();
}

uint64_t RemoteProcessStreamSource::get_sample_duration_us()
{
    // 保持稳定的 RTP 时间轴。
    // 可变节奏（空闲 FPS 变化）会增加浏览器端抖动缓冲堆积，
    // 可能表现为可见闪烁或黑帧。
    if (m_fps > 0) return 1000000ull / static_cast<uint64_t>(m_fps);
    return 0;
}

