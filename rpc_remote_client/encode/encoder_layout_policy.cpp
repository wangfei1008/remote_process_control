#include "encode/encoder_layout_policy.h"

#include <algorithm>
#include <cmath>

void EncoderLayoutPolicy::configure(int change_threshold_px, int required_streak)
{
    m_layout_change_threshold_px = (std::max)(0, change_threshold_px);
    m_layout_change_required_streak = (std::max)(1, required_streak);
}

void EncoderLayoutPolicy::reset()
{
    m_layout_change_streak = 0;
    m_pending_layout_w = 0;
    m_pending_layout_h = 0;
}

bool EncoderLayoutPolicy::should_apply_layout_change(int captured_w, int captured_h,
                                                     int current_w, int current_h,
                                                     bool had_successful_video)
{
    if (captured_w <= 0 || captured_h <= 0) return false;
    if (current_w <= 0 || current_h <= 0) return true;
    const int diff_w = std::abs(captured_w - current_w);
    const int diff_h = std::abs(captured_h - current_h);
    const bool diff_enough = (diff_w > m_layout_change_threshold_px) || (diff_h > m_layout_change_threshold_px);
    if (!diff_enough) {
        reset();
        return false;
    }

    if (m_pending_layout_w != captured_w || m_pending_layout_h != captured_h) {
        m_pending_layout_w = captured_w;
        m_pending_layout_h = captured_h;
        m_layout_change_streak = 1;
    } else {
        ++m_layout_change_streak;
    }

    // 首帧前允许较快切换；出画后继续允许动态切换，但要求更稳的连续命中，避免抖动重建编码器。
    const int required_streak = had_successful_video
        ? (std::max)(m_layout_change_required_streak, m_layout_change_required_streak * 2)
        : m_layout_change_required_streak;
    if (m_layout_change_streak >= required_streak) {
        reset();
        return true;
    }
    return false;
}

