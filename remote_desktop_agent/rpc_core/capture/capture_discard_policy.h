#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

class CaptureDiscardPolicy {
public:
    static bool should_discard_if_capture_too_slow(
        bool had_successful_video,
        bool have_last_good_sample,
        std::chrono::steady_clock::time_point t_cap_begin,
        std::chrono::steady_clock::time_point t_after_cap,
        int& out_capture_ms);

    static bool should_discard_if_empty_frame(const std::vector<uint8_t>& frame, int width, int height);
};

