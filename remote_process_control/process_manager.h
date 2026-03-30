#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "stream.h"
#include "window_capture.h"
#include "dxgi_capture.h"
#include "h264_encoder.hpp"
#include <chrono>

class ProcessManager : public StreamSource
{
    // SPS/PPS parsed from encoder extradata (avcC) as length-prefixed NAL units
    std::optional<std::vector<std::byte>> m_extradata_spspps = std::nullopt;
public:
    ProcessManager();
    ~ProcessManager();
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
    bool get_last_capture_used_hw() const { return m_last_capture_used_hw; }
    /** Whether DXGI is disabled for the current session due to repeated corruption. */
    bool is_dxgi_disabled_for_session() const { return m_disable_dxgi_for_session; }
    /** Current consecutive streak for detected top-black-strip corruption pattern. */
    int get_top_black_strip_streak() const { return m_top_black_strip_streak; }
    /** Session-level DXGI instability score (higher means more frequent DXGI failures). */
    int get_dxgi_instability_score() const { return m_dxgi_instability_score; }
    /** Unix ms until which software capture is forced. 0 means no forced software window. */
    uint64_t get_force_software_capture_until_unix_ms() const {
        return m_force_software_capture_until_unix_ms.load(std::memory_order_relaxed);
    }

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
    // When capture is continuously abnormal, avoid forcing IDR every single frame.
    // This reduces decoder spikes that can manifest as flicker/black bars.
    void request_force_keyframe_with_cooldown();
    /**
     * When captured resolution jitters slightly, avoid recreating encoder every frame.
     * We only apply layout change after:
     * 1) captured width/height differ enough (threshold), and
     * 2) the same new resolution is observed for N consecutive captures.
     */
    bool should_apply_layout_change(int capturedW, int capturedH);
    void reset_layout_change_tracking();
    std::vector<uint8_t> capture_main_window_image(HWND hwnd, int& outWidth, int& outHeight, int& outMinLeft, int& outMinTop);
    uint64_t quick_frame_signature(const std::vector<uint8_t>& frame, int width, int height) const;

    /** Process exit / HWND validity; return false to stop before capture. */
    bool tick_window_and_health(std::chrono::steady_clock::time_point& last_no_window_diag);
    void emit_hold_or_empty_sample(bool request_idr);
    bool decide_use_hw_capture(uint64_t cap_now_ms);
    bool grab_rgb_frame(int& width, int& height, int& cap_min_left, int& cap_min_top, std::vector<uint8_t>& frame,
                        bool use_hw_capture);
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
    AVCodecContext* m_av_codec_ctx = nullptr;
    HWND m_mainWindow = nullptr;
    std::string m_targetExeBaseName;
    int m_width;
    int m_height;
    int m_fps;
    int m_active_fps = 30;
    int m_idle_fps = 15;
    int m_current_fps = 30;
    int m_idle_enter_stable_frames = 24;
    int m_recent_input_boost_ms = 5000;
    int m_stable_frame_count = 0;
    uint64_t m_last_frame_sig = 0;
    bool m_has_last_sig = false;
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
    bool running = false;
    WindowCapture m_windowCapture; // Window image capture backend.
    DXGICapture m_dxgiCapture;
    rtc::binary sample;
    uint64_t sampleTime_us = 0;
    // When capture/encode fails intermittently, re-send the last good encoded frame
    // to avoid browser receiver showing black frames due to missing pictures.
    rtc::binary m_last_good_sample;
    bool m_have_last_good_sample = false;
    // Keep last good RGB frame for lightweight in-place repair (e.g. top black strip).
    std::vector<uint8_t> m_last_good_rgb_frame;
    int m_last_good_rgb_w = 0;
    int m_last_good_rgb_h = 0;
    std::atomic<bool> m_pending_force_keyframe{ false };
    // Unix timestamp in ms: last time we actually allowed a keyframe request.
    std::atomic<uint64_t> m_last_force_keyframe_unix_ms{ 0 };
    // When DXGI capture becomes unstable, temporarily force software capture (GDI).
    std::atomic<uint64_t> m_force_software_capture_until_unix_ms{ 0 };
    uint32_t m_capture_degrade_ms = 2500;
    // Black-strip specific safeguards for DXGI path.
    int m_top_black_strip_streak = 0;
    int m_top_black_strip_force_sw_threshold = 1;
    int m_top_black_strip_disable_dxgi_threshold = 3;
    bool m_disable_dxgi_for_session = false;
    bool m_last_capture_used_hw = false;
    // Session-level DXGI instability score/hysteresis.
    // Accumulate on DXGI empty/slow frames, decay on healthy DXGI frames.
    int m_dxgi_instability_score = 0;
    int m_dxgi_disable_score_threshold = 5;

    int m_dxgi_fail_streak = 0;
    int m_dxgi_fail_reset_threshold = 6;
    /** Monotonic per-encoder frame sequence used as PTS input. */
    int64_t m_encode_frame_seq = 0;

    uint32_t m_last_capture_ms = 0;
    uint32_t m_last_encode_ms = 0;
    uint64_t m_last_frame_unix_ms = 0;

    // Layout stability tracking (encoder resolution).
    // Avoid frequent encoder recreation due to tiny client-rect jitter.
    int m_layout_change_threshold_px = 8;           // ignore small 1~2px jitter
    int m_layout_change_required_streak = 5;       // require consecutive observations
    int m_layout_change_streak = 0;
    int m_pending_layout_w = 0;
    int m_pending_layout_h = 0;

    std::function<void()> m_on_remote_exit;
    bool m_had_successful_video = false;
    bool m_exit_notified = false;
};
