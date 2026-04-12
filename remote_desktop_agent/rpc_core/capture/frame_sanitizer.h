#pragma once

#include <vector>

class FrameSanitizer {
public:
    static bool sanitize_frame(std::vector<uint8_t>& frame,
                               int& width,
                               int& height,
                               bool had_successful_video,
                               bool have_last_good_sample,
                               const std::vector<uint8_t>& last_good_rgb_frame,
                               int last_good_rgb_w,
                               int last_good_rgb_h);
};
