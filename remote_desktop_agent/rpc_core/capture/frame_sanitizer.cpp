#include "capture/frame_sanitizer.h"

#include "app/runtime_config.h"
#include "capture/capture_rgb_heuristics.h"
#include "common/rpc_time.h"

#include <chrono>
#include <iostream>

bool FrameSanitizer::sanitize_frame(std::vector<uint8_t>& frame,
                                    int& width,
                                    int& height,
                                    int& cap_min_left,
                                    int& cap_min_top,
                                    bool had_successful_video,
                                    bool have_last_good_sample,
                                    bool session_uses_dxgi,
                                    const std::vector<uint8_t>& last_good_rgb_frame,
                                    int last_good_rgb_w,
                                    int last_good_rgb_h,
                                    GdiCapture& gdi_capture,
                                    CaptureBackendState& capture_backend_state)
{
    (void)cap_min_left;
    (void)cap_min_top;
    (void)gdi_capture;

    const bool filter_black = runtime_config::get_bool("RPC_FILTER_CAPTURE_BLACK_FRAMES", true);

    if (!(had_successful_video && have_last_good_sample)) {
        if (!filter_black) return true;
        if (frame.empty() || width <= 0 || height <= 0) return true;
        if (capture_rgb::is_suspicious_capture_frame(frame, width, height)) {
            static auto s_last_log = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (now - s_last_log > std::chrono::seconds(2)) {
                s_last_log = now;
                std::cout << "[capture] discard suspicious frame before first video (e.g. black startup)\n";
            }
            return false;
        }
        return true;
    }

    if (!filter_black) return true;

    const bool has_top_black_strip = capture_rgb::frame_has_top_black_strip_rgb24(frame, width, height);
    bool suspicious = capture_rgb::is_suspicious_capture_frame(frame, width, height);

    if (has_top_black_strip) {
        const bool can_repair_with_prev =
            (!last_good_rgb_frame.empty() && last_good_rgb_w == width && last_good_rgb_h == height &&
             last_good_rgb_frame.size() == frame.size());
        if (can_repair_with_prev) {
            capture_rgb::repair_top_strip_from_previous(frame, width, height, last_good_rgb_frame);
            suspicious = capture_rgb::is_suspicious_capture_frame(frame, width, height);
        }
        capture_backend_state.on_top_black_strip_detected(rpc_unix_epoch_ms(), session_uses_dxgi);
    } else {
        capture_backend_state.on_no_top_black_strip();
    }

    return !suspicious;
}
