#include "capture/capture_discard_policy.h"

#include <chrono>
#include <iostream>

bool CaptureDiscardPolicy::should_discard_if_capture_too_slow(
    bool had_successful_video,
    bool have_last_good_sample,
    std::chrono::steady_clock::time_point t_cap_begin,
    std::chrono::steady_clock::time_point t_after_cap,
    int& out_capture_ms)
{
    out_capture_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t_after_cap - t_cap_begin).count());

    if (!(had_successful_video && have_last_good_sample)) return false;

    constexpr int k_max_capture_ms = 80;
    if (out_capture_ms < k_max_capture_ms) return false;

    static auto s_last_slow_log = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last_slow_log > std::chrono::seconds(1)) {
        s_last_slow_log = now;
        std::cout << "[capture] slow capture " << out_capture_ms << "ms, discard frame\n";
    }
    return true;
}

bool CaptureDiscardPolicy::should_discard_if_empty_frame(const std::vector<uint8_t>& frame, int width, int height)
{
    return frame.empty() || width <= 0 || height <= 0;
}

