#include "source/silence_opus_source.h"

SilenceOpusSource::SilenceOpusSource(uint64_t frame_duration_us)
    : m_frame_duration_us(frame_duration_us)
{
    // 按 RFC 6716 生成舒适噪声（静音）数据包
    const std::byte pkt[] = { std::byte{0xF8}, std::byte{0xFF}, std::byte{0xFE} };
    m_sample = rtc::binary(std::begin(pkt), std::end(pkt));
}

void SilenceOpusSource::start()
{
    m_running = true;
    m_sample_time_us = 0;
}

void SilenceOpusSource::stop()
{
    m_running = false;
}

void SilenceOpusSource::load_next_sample()
{
    if (!m_running) return;
    m_sample_time_us += m_frame_duration_us;
}

rtc::binary SilenceOpusSource::get_sample()
{
    return m_sample;
}

uint64_t SilenceOpusSource::get_sample_time_us()
{
    return m_sample_time_us;
}

uint64_t SilenceOpusSource::get_sample_duration_us()
{
    return m_frame_duration_us;
}

