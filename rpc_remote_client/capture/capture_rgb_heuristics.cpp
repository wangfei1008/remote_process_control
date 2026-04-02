#include "capture/capture_rgb_heuristics.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace capture_rgb {

bool frame_mostly_black_rgb24(const std::vector<uint8_t>& rgb, int width, int height)
{
    if (rgb.empty() || width <= 0 || height <= 0) return false;
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    if (rgb.size() < expected) return false;

    const int sampleRows = (std::min)(height, 24);
    const int sampleCols = (std::min)(width, 48);
    const int rowStep = (std::max)(1, height / sampleRows);
    const int colStep = (std::max)(1, width / sampleCols);

    int total = 0;
    int blackish = 0;
    for (int y = 0; y < height; y += rowStep) {
        const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        for (int x = 0; x < width; x += colStep) {
            const size_t idx = rowBase + static_cast<size_t>(x) * 3u;
            if (idx + 2 >= rgb.size()) break;
            const int r = rgb[idx + 0];
            const int g = rgb[idx + 1];
            const int b = rgb[idx + 2];
            const int sum = r + g + b;
            total++;
            if (sum <= 24) blackish++;
        }
    }
    if (total <= 0) return false;
    const double ratio = static_cast<double>(blackish) / static_cast<double>(total);
    return ratio >= 0.90;
}

bool frame_low_variance_rgb24(const std::vector<uint8_t>& rgb, int width, int height)
{
    if (rgb.empty() || width <= 0 || height <= 0) return false;
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    if (rgb.size() < expected) return false;

    const int sampleRows = (std::min)(height, 24);
    const int sampleCols = (std::min)(width, 48);
    const int rowStep = (std::max)(1, height / sampleRows);
    const int colStep = (std::max)(1, width / sampleCols);

    double sum = 0.0;
    double sumSq = 0.0;
    int n = 0;
    for (int y = 0; y < height; y += rowStep) {
        const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        for (int x = 0; x < width; x += colStep) {
            const size_t idx = rowBase + static_cast<size_t>(x) * 3u;
            if (idx + 2 >= rgb.size()) break;
            const double luma = 0.299 * rgb[idx + 0] + 0.587 * rgb[idx + 1] + 0.114 * rgb[idx + 2];
            sum += luma;
            sumSq += luma * luma;
            n++;
        }
    }
    if (n <= 0) return false;
    const double mean = sum / n;
    const double var = (sumSq / n) - (mean * mean);
    return (var < 12.0);
}

bool frame_has_top_black_strip_rgb24(const std::vector<uint8_t>& rgb, int width, int height)
{
    if (rgb.empty() || width <= 0 || height <= 0) return false;
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    if (rgb.size() < expected) return false;
    if (height < 24 || width < 48) return false;

    const int topH = (std::max)(10, height / 6);
    const int sampleCols = (std::min)(width, 96);
    const int colStep = (std::max)(1, width / sampleCols);

    int topTotal = 0;
    int topBlackish = 0;
    for (int y = 0; y < topH; ++y) {
        const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        for (int x = 0; x < width; x += colStep) {
            const size_t idx = rowBase + static_cast<size_t>(x) * 3u;
            if (idx + 2 >= rgb.size()) break;
            const int r = rgb[idx + 0];
            const int g = rgb[idx + 1];
            const int b = rgb[idx + 2];
            if ((r + g + b) <= 24) topBlackish++;
            topTotal++;
        }
    }
    if (topTotal <= 0) return false;
    const double topRatio = static_cast<double>(topBlackish) / static_cast<double>(topTotal);
    if (topRatio < 0.80) return false;

    const int bottomY0 = (std::max)(topH, height / 2);
    int lowerTotal = 0;
    int lowerBlackish = 0;
    for (int y = bottomY0; y < height; ++y) {
        const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
        for (int x = 0; x < width; x += colStep) {
            const size_t idx = rowBase + static_cast<size_t>(x) * 3u;
            if (idx + 2 >= rgb.size()) break;
            const int r = rgb[idx + 0];
            const int g = rgb[idx + 1];
            const int b = rgb[idx + 2];
            if ((r + g + b) <= 24) lowerBlackish++;
            lowerTotal++;
        }
    }
    if (lowerTotal <= 0) return false;
    const double lowerRatio = static_cast<double>(lowerBlackish) / static_cast<double>(lowerTotal);
    return lowerRatio < 0.75;
}

bool is_suspicious_capture_frame(const std::vector<uint8_t>& rgb, int width, int height)
{
    // 统一可疑帧入口：这里保持“快判断”，不做昂贵图像算法。
    // 真正高成本兜底（如二次重采）由上层按需触发。
    if (rgb.empty() || width <= 0 || height <= 0) return true;
    if (frame_mostly_black_rgb24(rgb, width, height)) return true;
    if (frame_low_variance_rgb24(rgb, width, height)) return true;
    if (frame_has_top_black_strip_rgb24(rgb, width, height)) return true;
    return false;
}

void repair_top_strip_from_previous(std::vector<uint8_t>& rgb, int width, int height,
                                    const std::vector<uint8_t>& prev_rgb)
{
    if (rgb.empty() || prev_rgb.size() != rgb.size() || width <= 0 || height <= 0) return;
    const int patchRows = (std::max)(10, height / 6);
    const size_t rowBytes = static_cast<size_t>(width) * 3u;
    for (int y = 0; y < patchRows; ++y) {
        const size_t off = static_cast<size_t>(y) * rowBytes;
        if (off + rowBytes > rgb.size()) break;
        std::memcpy(rgb.data() + off, prev_rgb.data() + off, rowBytes);
    }
}

}
