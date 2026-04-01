#pragma once

#include <cstdint>
#include <vector>

class FramePacingPolicy {
public:
    void reset(int active_fps, int idle_fps, int idle_enter_stable_frames);
    int update_and_get_fps(const std::vector<uint8_t>& frame, int width, int height, bool recent_input);

private:
    static uint64_t compute_frame_signature(const std::vector<uint8_t>& frame, int width, int height);

    int m_active_fps = 30;
    int m_idle_fps = 5;
    int m_idle_enter_stable_frames = 24;
    int m_stable_frame_count = 0;
    uint64_t m_last_frame_sig = 0;
    bool m_has_last_sig = false;
};

