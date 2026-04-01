#include "capture/frame_sanitizer.h"

#include "capture/capture_rgb_heuristics.h"
#include "common/rpc_time.h"
#include "capture/window_frame_grabber.h"

#include <chrono>
#include <iostream>

bool FrameSanitizer::sanitize_frame(std::vector<uint8_t>& frame,
                                    int& width,
                                    int& height,
                                    int& cap_min_left,
                                    int& cap_min_top,
                                    bool had_successful_video,
                                    bool have_last_good_sample,
                                    bool hw_capture_active,
                                    bool capture_all_windows,
                                    HWND main_window,
                                    const std::vector<uint8_t>& last_good_rgb_frame,
                                    int last_good_rgb_w,
                                    int last_good_rgb_h,
                                    GdiCapture& gdi_capture,
                                    CaptureBackendState& capture_backend_state)
{
    if (!(had_successful_video && have_last_good_sample)) return true;

    const bool has_top_black_strip = capture_rgb::frame_has_top_black_strip_rgb24(frame, width, height);
    bool suspicious = capture_rgb::is_suspicious_capture_frame(frame, width, height);

    if (has_top_black_strip) {
        const bool can_repair_with_prev =
            (!last_good_rgb_frame.empty() &&
             last_good_rgb_w == width &&
             last_good_rgb_h == height &&
             last_good_rgb_frame.size() == frame.size());
        if (can_repair_with_prev) {
            capture_rgb::repair_top_strip_from_previous(frame, width, height, last_good_rgb_frame);
            suspicious = capture_rgb::is_suspicious_capture_frame(frame, width, height);
        }
        capture_backend_state.on_top_black_strip_detected(rpc_unix_epoch_ms(), hw_capture_active);
    } else {
        capture_backend_state.on_no_top_black_strip();
    }

    if (suspicious && hw_capture_active && !capture_all_windows && main_window && IsWindow(main_window)) {
        int sw = 0, sh = 0, sl = 0, st = 0;
        auto fallback_frame = WindowFrameGrabber::capture_main_window_image(
            gdi_capture, main_window, sw, sh, sl, st, true);
        if (!fallback_frame.empty() && sw > 0 && sh > 0 &&
            !capture_rgb::is_suspicious_capture_frame(fallback_frame, sw, sh)) {
            frame.swap(fallback_frame);
            width = sw;
            height = sh;
            cap_min_left = sl;
            cap_min_top = st;
            suspicious = false;
            static auto s_last_recover_log = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - s_last_recover_log > std::chrono::seconds(1)) {
                s_last_recover_log = now;
                std::cout << "[capture] suspicious dxgi frame recovered by software recapture\n";
            }
        }
    }

    return !suspicious;
}

