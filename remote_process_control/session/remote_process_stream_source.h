#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "source/stream.h"
#include "capture/gdi_capture.h"
#include "capture/dxgi_capture.h"
#include "capture/bmp_dump_writer.h"
#include "session/remote_process_session.h"
#include "capture/capture_backend_state.h"
#include "encode/video_encode_pipeline.h"
#include "session/sample_output_state.h"
#include <chrono>

class RemoteProcessStreamSource : public StreamSource
{
public:
    RemoteProcessStreamSource();
    ~RemoteProcessStreamSource();
    HWND launch_process(const std::string& exe_path);
    void terminate();

    // Called by receiver (frontend) when it detects sustained packet loss/jitter.
    // Forces the next encoded video to include an IDR/keyframe for faster decoder recovery.
    void request_force_keyframe();

    // StreamSource interface implementation.
    void start() override;
    void stop() override;
    void load_next_sample() override;
    rtc::binary get_sample() override;
    uint64_t get_sample_time_us() override;
    uint64_t get_sample_duration_us() override;

    HWND get_main_window() const { return m_mainWindow; }
    /** Captured frame width and height used by the current encoder instance. */
    int get_capture_width() const { return m_width; }
    int get_capture_height() const { return m_height; }
    /** Last capture time in milliseconds. */
    uint32_t get_last_capture_ms() const { return m_last_capture_ms; }
    /** Last H.264 encode time in milliseconds. */
    uint32_t get_last_encode_ms() const { return m_last_encode_ms; }
    /** Unix timestamp (ms) for the most recently produced video frame. */
    uint64_t get_last_frame_unix_ms() const { return m_last_frame_unix_ms; }
    /** Whether the latest capture path used DXGI hardware duplication. */
    bool get_last_capture_used_hw() const { return m_capture_backend_state.get_last_capture_used_hw(); }
    /** Whether DXGI is disabled for the current session due to repeated corruption. */
    bool is_dxgi_disabled_for_session() const { return m_capture_backend_state.is_dxgi_disabled_for_session(); }
    /** Current consecutive streak for detected top-black-strip corruption pattern. */
    int get_top_black_strip_streak() const { return m_capture_backend_state.get_top_black_strip_streak(); }
    /** Session-level DXGI instability score (higher means more frequent DXGI failures). */
    int get_dxgi_instability_score() const { return m_capture_backend_state.get_dxgi_instability_score(); }
    /** Unix ms until which software capture is forced. 0 means no forced software window. */
    uint64_t get_force_software_capture_until_unix_ms() const {
        return m_capture_backend_state.get_force_software_capture_until_unix_ms();
    }

    /** Set callback for remote process/window exit notifications. */
    void set_on_remote_exit(std::function<void()> cb) { m_on_remote_exit = std::move(cb); }

private:
    void notify_remote_exit();
    /** Process exit / HWND validity; return false to stop before capture. */
    bool tick_window_and_health(std::chrono::steady_clock::time_point& last_no_window_diag);
    BmpDumpDiag make_dump_diag(bool use_hw_capture) const;
    void emit_hold_or_empty_sample(bool request_idr);
    bool decide_use_hw_capture(uint64_t cap_now_ms);
    bool grab_rgb_frame(int& width, int& height, int& cap_min_left, int& cap_min_top, std::vector<uint8_t>& frame,
                        bool use_hw_capture, bool& used_hw_capture_out);
    bool discard_if_capture_too_slow(std::chrono::steady_clock::time_point t_cap_begin,
                                    std::chrono::steady_clock::time_point t_after_cap, bool use_hw_capture);
    bool discard_if_empty_frame(const std::vector<uint8_t>& frame, int width, int height);
    bool filter_suspicious_frame(std::vector<uint8_t>& frame, int& width, int& height, int& cap_min_left,
                                 int& cap_min_top);
    void ensure_encoder_layout(int captured_w, int captured_h, bool& applied_layout_out);
    void update_fps_pacing_and_mapping(const std::vector<uint8_t>& frame, int captured_w, int captured_h,
                                       int cap_min_left, int cap_min_top);
    void finalize_encode_rgb(std::vector<uint8_t>& frame, int captured_w, int captured_h,
                             std::chrono::steady_clock::time_point t_cap_begin,
                             std::chrono::steady_clock::time_point t_after_cap, bool applied_layout);

    PROCESS_INFORMATION m_pi;
    DWORD m_launchPid = 0;   // PID returned by CreateProcess
    DWORD m_capturePid = 0;  // PID that actually owns the window we capture
    HWND m_mainWindow = nullptr;
    std::string m_targetExeBaseName;
    int m_width;
    int m_height;
    int m_fps;
    int m_active_fps = 30;
    int m_idle_fps = 15;
    bool m_capture_all_windows = false;
    bool m_hw_capture_supported = false;
    bool m_hw_capture_requested = false;
    bool m_hw_capture_active = false;
    bool m_lock_capture_backend = false;
    bool m_locked_use_hw_capture = false;
    bool m_allow_pid_rebind_by_exename = true;
    uint64_t m_pid_rebind_deadline_unix_ms = 0;
    uint64_t m_window_missing_since_unix_ms = 0;
    uint32_t m_window_missing_exit_grace_ms = 5000;
    bool m_running = false;
    GdiCapture m_gdiCapture; // GDI window image capture backend.
    DXGICapture m_dxgiCapture;
    SampleOutputState m_sample_output_state;
    // Keep last good RGB frame for lightweight in-place repair (e.g. top black strip).
    std::vector<uint8_t> m_last_good_rgb_frame;
    int m_last_good_rgb_w = 0;
    int m_last_good_rgb_h = 0;
    uint32_t m_capture_degrade_ms = 2500;

    uint32_t m_last_capture_ms = 0;
    uint32_t m_last_encode_ms = 0;
    uint64_t m_last_frame_unix_ms = 0;

    std::function<void()> m_on_remote_exit;
    bool m_had_successful_video = false;
    bool m_exit_notified = false;

    BmpDumpWriter m_bmp_dump_writer;
    RemoteProcessSession m_remote_process_session;
    CaptureBackendState m_capture_backend_state;
    VideoEncodePipeline m_video_encode_pipeline;
};
