#include "encode/encoder_layout_policy.h"

#include <algorithm>
#include <cmath>

void EncoderLayoutPolicy::reset()
{
    m_layout_change_streak = 0;
    m_pending_layout_w = 0;
    m_pending_layout_h = 0;
}

bool EncoderLayoutPolicy::should_apply_layout_change(int captured_w, int captured_h, int current_w, int current_h)
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

    if (m_layout_change_streak >= m_layout_change_required_streak) {
        reset();
        return true;
    }
    return false;
}

