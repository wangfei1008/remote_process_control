#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <windows.h>
#include <vector>

#include "rtc/rtc.hpp"

#include "capture/bmp_dump_writer.h"
#include "capture/process_ui_capture.h"
#include "session/remote_capture_telemetry.h"

class RemoteProcessSession;

class GdiCapture;
class DXGICapture;
class CaptureBackendState;
class FrameSanitizer;
class CaptureDiscardPolicy;
class VideoEncodePipeline;

// 视频引擎：进程全界面采集（不完整帧丢弃）+ 编码
class remote_video_engine {
public:
    using window_missing_fn = std::function<void(const char* why, uint64_t missing_ms)>;
    remote_video_engine(std::string exe_path,
                        std::function<void()> on_remote_process_exit,
                        window_missing_fn on_window_missing);
    ~remote_video_engine();

    void start();
    void stop();

    void produce_next_video_sample(rtc::binary& out_sample, remote_capture_telemetry& out_telemetry);

    void request_force_keyframe();
    HWND get_main_window() const;

private:
    void notify_remote_exit_if_needed(const char* why);
    void notify_window_missing_if_needed(const char* why, uint64_t now_unix_ms);
    bool is_remote_process_still_running() const;
    void exit_watch_loop();

    void apply_emit_fail_policy(rtc::binary& out_sample, bool request_idr);

    void reset_for_session_start();
    void try_recover_main_window(uint64_t now_unix_ms);
    void fill_capture_backend_telemetry(remote_capture_telemetry& out_telemetry) const;

    std::string m_exe_path;
    std::function<void()> m_on_remote_process_exit;
    window_missing_fn m_on_window_missing;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_exit_notified{false};

    PROCESS_INFORMATION m_pi{};
    DWORD m_capture_pid = 0;
    DWORD m_launch_pid = 0;
    HWND m_main_window = nullptr;
    bool m_capture_backend_is_auto = true;

    std::thread m_exit_watch_thread;

    std::unique_ptr<RemoteProcessSession> m_process_session;
    std::string m_target_exe_base_name;
    bool m_allow_pid_rebind_by_exename = true;
    uint64_t m_pid_rebind_deadline_unix_ms = 0;

    bool m_session_uses_dxgi = false;
    uint32_t m_capture_degrade_ms = 2500;
    ProcessUiCaptureOptions m_ui_capture_options;

    std::unique_ptr<GdiCapture> m_gdi_capture;
    std::unique_ptr<DXGICapture> m_dxgi_capture;
    std::unique_ptr<CaptureBackendState> m_capture_backend_state;

    int m_video_fps = 30;
    int m_encoder_layout_change_threshold_px = 8;
    int m_encoder_layout_change_required_streak = 5;

    std::unique_ptr<VideoEncodePipeline> m_video_encode_pipeline;
    BmpDumpWriter m_bmp_dump;
    bool m_had_successful_video = false;

    bool m_steady_frame_hold = false;

    rtc::binary m_last_good_video_sample;
    bool m_have_last_good_sample = false;

    std::vector<uint8_t> m_last_good_rgb_frame;
    int m_last_good_rgb_w = 0;
    int m_last_good_rgb_h = 0;

    uint64_t m_window_missing_since_unix_ms = 0;
    uint32_t m_window_missing_exit_grace_ms = 0;
    uint64_t m_last_window_missing_notify_unix_ms = 0;
};
