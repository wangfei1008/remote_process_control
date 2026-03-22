#pragma once

#include "stream.h"
#include <cstdint>

// Minimal Opus "silence" source.
// Uses RFC 6716 comfort noise packet 0xF8 0xFF 0xFE at 20ms intervals.
class SilenceOpusSource : public StreamSource
{
public:
    explicit SilenceOpusSource(uint64_t frame_duration_us = 20000);

    void start() override;
    void stop() override;
    void load_next_sample() override;
    rtc::binary get_sample() override;
    uint64_t get_sample_time_us() override;
    uint64_t get_sample_duration_us() override;

private:
    bool m_running = false;
    uint64_t m_sample_time_us = 0;
    uint64_t m_frame_duration_us = 20000;
    rtc::binary m_sample;
};

