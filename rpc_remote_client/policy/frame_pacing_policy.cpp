#include "policy/frame_pacing_policy.h"

#include <algorithm>

void FramePacingPolicy::reset(int active_fps, int idle_fps, int idle_enter_stable_frames)
{
    m_active_fps = (std::max)(1, active_fps);
    m_idle_fps = (std::max)(1, (std::min)(idle_fps, m_active_fps));
    m_idle_enter_stable_frames = (std::max)(1, idle_enter_stable_frames);
    m_stable_frame_count = 0;
    m_last_frame_sig = 0;
    m_has_last_sig = false;
}

int FramePacingPolicy::update_and_get_fps(const std::vector<uint8_t>& frame, int width, int height, bool recent_input)
{
    const uint64_t frame_sig = compute_frame_signature(frame, width, height);
    const bool frame_changed = (!m_has_last_sig || frame_sig != m_last_frame_sig);
    m_last_frame_sig = frame_sig;
    m_has_last_sig = true;

    if (frame_changed || recent_input) {
        m_stable_frame_count = 0;
        return m_active_fps;
    }

    ++m_stable_frame_count;
    if (m_stable_frame_count >= m_idle_enter_stable_frames) {
        return m_idle_fps;
    }
    return m_active_fps;
}

uint64_t FramePacingPolicy::compute_frame_signature(const std::vector<uint8_t>& frame, int width, int height)
{
    if (frame.empty() || width <= 0 || height <= 0) return 0;

    constexpr int k_bytes_per_pixel = 3;
    const size_t row_stride = static_cast<size_t>(width) * k_bytes_per_pixel;
    const int sample_rows = (std::min)(height, 32);
    const int sample_cols = (std::min)(width, 64);
    const int row_step = (std::max)(1, height / sample_rows);
    const int col_step = (std::max)(1, width / sample_cols);

    uint64_t sig = 1469598103934665603ull;
    for (int y = 0; y < height; y += row_step) {
        const size_t row_base = static_cast<size_t>(y) * row_stride;
        for (int x = 0; x < width; x += col_step) {
            const size_t idx = row_base + static_cast<size_t>(x) * k_bytes_per_pixel;
            if (idx + 2 >= frame.size()) break;
            sig ^= static_cast<uint64_t>(frame[idx + 0]); sig *= 1099511628211ull;
            sig ^= static_cast<uint64_t>(frame[idx + 1]); sig *= 1099511628211ull;
            sig ^= static_cast<uint64_t>(frame[idx + 2]); sig *= 1099511628211ull;
        }
    }
    return sig;
}

