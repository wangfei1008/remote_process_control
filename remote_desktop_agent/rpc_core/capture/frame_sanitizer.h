#pragma once

#include <windows.h>

#include <vector>

#include "capture/capture_backend_state.h"
#include "capture/gdi_capture.h"

class FrameSanitizer {
public:
    static bool sanitize_frame(std::vector<uint8_t>& frame,
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
                               CaptureBackendState& capture_backend_state);
};
