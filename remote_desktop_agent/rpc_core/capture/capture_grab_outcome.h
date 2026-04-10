#pragma once

#include <cstdint>
#include <vector>

struct CaptureGrabOutcome {
    bool ok = true;
    bool need_hold_on_empty_fallback = false;
    bool used_hw_capture = false;
    int width = 0;
    int height = 0;
    int cap_min_left = 0;
    int cap_min_top = 0;
    std::vector<uint8_t> frame;
};
