#pragma once

#include <cstdint>
#include <windows.h>

#include "rtc/rtc.hpp"

// 静音 Opus 生成器：阶段性接口（输出固定 RFC 6716 comfort noise）。
class silence_opus_generator {
public:
    explicit silence_opus_generator(uint64_t frame_duration_us = 20000);

    void start();
    void stop();

    // 生成下一段音频样本（内部不做重编码，当前阶段输出固定静音样本）。
    void produce_next_audio_sample(rtc::binary& out_sample);

    uint64_t frame_duration_us() const { return m_frame_duration_us; }

private:
    bool m_running = false;
    uint64_t m_frame_duration_us = 20000;
    rtc::binary m_sample;
};

