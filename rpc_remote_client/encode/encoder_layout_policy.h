#pragma once

class EncoderLayoutPolicy {
public:
    void configure(int change_threshold_px, int required_streak);
    void reset();
    bool should_apply_layout_change(int captured_w, int captured_h, int current_w, int current_h, bool had_successful_video);

private:
    int m_layout_change_threshold_px = 8;
    int m_layout_change_required_streak = 5;
    int m_layout_change_streak = 0;
    int m_pending_layout_w = 0;
    int m_pending_layout_h = 0;
};

