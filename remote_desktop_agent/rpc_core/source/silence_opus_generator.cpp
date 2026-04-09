#include "source/silence_opus_generator.h"

silence_opus_generator::silence_opus_generator(uint64_t frame_duration_us)
    : m_frame_duration_us(frame_duration_us)
{
    // RFC 6716：舒适噪声包（0xF8 0xFF 0xFE）
    const std::byte pkt[] = { std::byte{0xF8}, std::byte{0xFF}, std::byte{0xFE} };
    m_sample = rtc::binary(std::begin(pkt), std::end(pkt));
}

void silence_opus_generator::start()
{
    m_running = true;
}

void silence_opus_generator::stop()
{
    m_running = false;
}

void silence_opus_generator::produce_next_audio_sample(rtc::binary& out_sample)
{
    if (!m_running) {
        out_sample.clear();
        return;
    }
    out_sample = m_sample;
}

