#pragma once
#include <cstdint>
#include <vector>

namespace capture_rgb {
bool frame_mostly_black_rgb24(const std::vector<uint8_t>& rgb, int width, int height);
bool frame_low_variance_rgb24(const std::vector<uint8_t>& rgb, int width, int height);
bool frame_has_top_black_strip_rgb24(const std::vector<uint8_t>& rgb, int width, int height);
bool is_suspicious_capture_frame(const std::vector<uint8_t>& rgb, int width, int height);
void repair_top_strip_from_previous(std::vector<uint8_t>& rgb, int width, int height,
                                    const std::vector<uint8_t>& prev_rgb);
} 
