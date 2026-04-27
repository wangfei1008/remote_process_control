// ============================================================
// frame_filter.cpp
// ============================================================

#include "frame_filter.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace capture {

// ---- 主判断 -------------------------------------------------

bool FrameFilter::should_discard(std::span<const uint8_t> rgb,
                                  int w, int h) const {
    if (rgb.empty() || w <= 0 || h <= 0) return true;
    if (is_mostly_black(rgb, w, h))    return true;
    if (is_low_variance(rgb, w, h))    return true;
    if (has_top_black_strip(rgb, w, h)) return true;
    return false;
}

// ---- 修复 ---------------------------------------------------

void FrameFilter::repair_top_strip(std::vector<uint8_t>&    rgb,
                                    int                      w,
                                    int                      h,
                                    std::span<const uint8_t> prev_rgb) const {
    if (rgb.empty() || prev_rgb.size() != rgb.size() || w <= 0 || h <= 0) return;
    const int patch_rows   = std::max(10, h / 6);
    const size_t row_bytes = static_cast<size_t>(w) * 3u;
    for (int y = 0; y < patch_rows; ++y) {
        const size_t off = static_cast<size_t>(y) * row_bytes;
        if (off + row_bytes > rgb.size()) break;
        std::memcpy(rgb.data() + off, prev_rgb.data() + off, row_bytes);
    }
}

// ---- 子检测 -------------------------------------------------

bool FrameFilter::is_mostly_black(std::span<const uint8_t> rgb,
                                   int w, int h) const {
    const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 3u;
    if (rgb.size() < expected) return false;

    const int row_step = std::max(1, h / std::min(h, 24));
    const int col_step = std::max(1, w / std::min(w, 48));

    int total = 0, blackish = 0;
    for (int y = 0; y < h; y += row_step) {
        const size_t row_base = static_cast<size_t>(y) * static_cast<size_t>(w) * 3u;
        for (int x = 0; x < w; x += col_step) {
            const size_t idx = row_base + static_cast<size_t>(x) * 3u;
            if (idx + 2 >= rgb.size()) break;
            if (rgb[idx]+rgb[idx+1]+rgb[idx+2] <= 24) ++blackish;
            ++total;
        }
    }
    if (total <= 0) return false;
    return (static_cast<double>(blackish) / total) >= 0.90;
}

bool FrameFilter::is_low_variance(std::span<const uint8_t> rgb,
                                   int w, int h) const {
    const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 3u;
    if (rgb.size() < expected) return false;

    const int row_step = std::max(1, h / std::min(h, 24));
    const int col_step = std::max(1, w / std::min(w, 48));

    double sum = 0.0, sum_sq = 0.0;
    int n = 0;
    for (int y = 0; y < h; y += row_step) {
        const size_t row_base = static_cast<size_t>(y) * static_cast<size_t>(w) * 3u;
        for (int x = 0; x < w; x += col_step) {
            const size_t idx = row_base + static_cast<size_t>(x) * 3u;
            if (idx + 2 >= rgb.size()) break;
            const double luma = 0.299*rgb[idx] + 0.587*rgb[idx+1] + 0.114*rgb[idx+2];
            sum    += luma;
            sum_sq += luma * luma;
            ++n;
        }
    }
    if (n <= 0) return false;
    const double mean = sum / n;
    const double var  = (sum_sq / n) - (mean * mean);
    return var < 12.0;
}

bool FrameFilter::has_top_black_strip(std::span<const uint8_t> rgb,
                                       int w, int h) const {
    const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 3u;
    if (rgb.size() < expected) return false;
    if (h < 24 || w < 48) return false;

    const int top_h    = std::max(10, h / 6);
    const int col_step = std::max(1, w / std::min(w, 96));

    // 顶部区域黑色比例
    int top_total = 0, top_black = 0;
    for (int y = 0; y < top_h; ++y) {
        const size_t row_base = static_cast<size_t>(y) * static_cast<size_t>(w) * 3u;
        for (int x = 0; x < w; x += col_step) {
            const size_t idx = row_base + static_cast<size_t>(x) * 3u;
            if (idx + 2 >= rgb.size()) break;
            if (rgb[idx]+rgb[idx+1]+rgb[idx+2] <= 24) ++top_black;
            ++top_total;
        }
    }
    if (top_total <= 0) return false;
    if (static_cast<double>(top_black) / top_total < 0.80) return false;

    // 下半区域黑色比例（应明显低于顶部）
    const int bottom_y0 = std::max(top_h, h / 2);
    int lower_total = 0, lower_black = 0;
    for (int y = bottom_y0; y < h; ++y) {
        const size_t row_base = static_cast<size_t>(y) * static_cast<size_t>(w) * 3u;
        for (int x = 0; x < w; x += col_step) {
            const size_t idx = row_base + static_cast<size_t>(x) * 3u;
            if (idx + 2 >= rgb.size()) break;
            if (rgb[idx]+rgb[idx+1]+rgb[idx+2] <= 24) ++lower_black;
            ++lower_total;
        }
    }
    if (lower_total <= 0) return false;
    return (static_cast<double>(lower_black) / lower_total) < 0.75;
}

} // namespace capture
