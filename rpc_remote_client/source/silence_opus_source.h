#pragma once

#include "source/stream.h"
#include <cstdint>

// 最小化 Opus 静音源实现。
// 以 20ms 间隔发送 RFC 6716 舒适噪声包 0xF8 0xFF 0xFE。
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

