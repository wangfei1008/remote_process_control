#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include "stream.h"
#include "window_capture.h"
#include "dxgi_capture.h"
#include "h264_encoder.hpp"

class ProcessManager : public StreamSource
{
    std::optional<std::vector<std::byte>> previousUnitType5 = std::nullopt;
    std::optional<std::vector<std::byte>> previousUnitType7 = std::nullopt;
    std::optional<std::vector<std::byte>> previousUnitType8 = std::nullopt;
    // SPS/PPS parsed from encoder extradata (avcC) as length-prefixed NAL units
    std::optional<std::vector<std::byte>> m_extradata_spspps = std::nullopt;
public:
    ProcessManager();
    ~ProcessManager();
    HWND launch_process(const std::string& exe_path);
    void terminate();

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

    std::vector<HWND> find_all_windows(DWORD pid);
    // Exposed for external capture/control coordination.
    /** Merge all visible windows of a process, optionally clipping below anchorHwnd by maxBelowMainPx. */
    std::vector<uint8_t> capture_all_windows_image(DWORD pid, HWND anchorHwnd, int maxBelowMainPx,
                                                   int& outWidth, int& outHeight, int& outMinLeft, int& outMinTop);

    /** Set callback for remote process/window exit notifications. */
    void set_on_remote_exit(std::function<void()> cb) { m_on_remote_exit = std::move(cb); }

private:
    void notify_remote_exit();
    HWND find_main_window(DWORD pid);
    static std::string basename_from_path(const std::string& path);
    static std::string get_process_basename(DWORD pid);
    HWND find_window_by_exe_basename(const std::string& exeBaseName);
    std::vector<uint8_t> capture_main_window_image(HWND hwnd, int& outWidth, int& outHeight, int& outMinLeft, int& outMinTop);
    uint64_t quick_frame_signature(const std::vector<uint8_t>& frame, int width, int height) const;
private:
    PROCESS_INFORMATION m_pi;
    DWORD m_launchPid = 0;   // PID returned by CreateProcess
    DWORD m_capturePid = 0;  // PID that actually owns the window we capture
    AVCodecContext* m_av_codec_ctx = nullptr;
    HWND m_mainWindow = nullptr;
    std::string m_targetExeBaseName;
    int m_width;
    int m_height;
    int m_fps;
    int m_active_fps = 30;
    int m_idle_fps = 5;
    int m_current_fps = 30;
    int m_idle_enter_stable_frames = 12;
    int m_recent_input_boost_ms = 2000;
    int m_stable_frame_count = 0;
    uint64_t m_last_frame_sig = 0;
    bool m_has_last_sig = false;
    bool m_capture_all_windows = false;
    bool m_hw_capture_supported = false;
    bool m_hw_capture_requested = false;
    bool m_hw_capture_active = false;
    bool running = false;
    WindowCapture m_windowCapture; // Window image capture backend.
    DXGICapture m_dxgiCapture;
    rtc::binary sample;
    uint64_t sampleTime_us = 0;
    /** Monotonic per-encoder frame sequence used as PTS input. */
    int64_t m_encode_frame_seq = 0;

    uint32_t m_last_capture_ms = 0;
    uint32_t m_last_encode_ms = 0;
    uint64_t m_last_frame_unix_ms = 0;

    std::function<void()> m_on_remote_exit;
    bool m_had_successful_video = false;
    bool m_exit_notified = false;
};
