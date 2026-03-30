#include "desktop_screen_source.h"


DesktopScreenSource::DesktopScreenSource(int width, int height, int fps, bool loop)
    : m_width(width)
	, m_height(height)
    , fps(fps)
    , m_capture()
    , loop(loop)
{
    m_av_codec_ctx = create_h264_encoder(width, height, fps);
}

void DesktopScreenSource::start() {
    running = true;
    sampleTime_us = 0;
    m_encode_frame_seq = 0;
}

void DesktopScreenSource::stop() {
    running = false;
}

void DesktopScreenSource::load_next_sample() 
{
    if (!running) return;
    std::vector<uint8_t> rbg_frame(m_width * m_height * 3);
    if (m_capture.grab_frame(rbg_frame.data(), m_width, m_height)) {
        // Encode the captured RGB frame into H264.
        std::vector<uint8_t> h264_data;
        // encode_rgb signature includes frame sequence and keyframe control.
        bool ok = encode_rgb(m_av_codec_ctx, rbg_frame.data(), m_width, m_height, m_encode_frame_seq, h264_data);
        if (ok && !h264_data.empty()) {
            std::vector<std::byte> h264_bytes(h264_data.size());
            std::memcpy(h264_bytes.data(), h264_data.data(), h264_data.size());
            sample = rtc::binary(h264_bytes.begin(), h264_bytes.end());
            sampleTime_us += get_sample_duration_us();

            size_t i = 0;
            while (i + 4 <= sample.size()) {
                uint32_t length;
                std::memcpy(&length, sample.data() + i, sizeof(uint32_t));
                length = ntohl(length);
                auto naluStartIndex = i + 4;
                auto naluEndIndex = naluStartIndex + length;
                if (naluEndIndex > sample.size()) break; // Invalid/truncated NAL unit length.
                auto header = reinterpret_cast<rtc::NalUnitHeader*>(sample.data() + naluStartIndex);
                auto type = header->unitType();
                switch (type) {
                case 7:
                    previousUnitType7 = { sample.begin() + i, sample.begin() + naluEndIndex };
                    break;
                case 8:
                    previousUnitType8 = { sample.begin() + i, sample.begin() + naluEndIndex };
                    break;
                case 5:
                    previousUnitType5 = { sample.begin() + i, sample.begin() + naluEndIndex };
                    break;
                }
                i = naluEndIndex;
            }
        }
    }
}

rtc::binary DesktopScreenSource::get_sample()
{
    return sample;
}

uint64_t DesktopScreenSource::get_sample_time_us()
{
    return sampleTime_us;
}

uint64_t DesktopScreenSource::get_sample_duration_us()
{
    return 1000000 / fps; // Frame duration in microseconds.
}

std::vector<std::byte> DesktopScreenSource::initial_nalus()
{
    std::vector<std::byte> units{};
    if (previousUnitType7.has_value()) {
        auto nalu = previousUnitType7.value();
        units.insert(units.end(), nalu.begin(), nalu.end());
    }
    if (previousUnitType8.has_value()) {
        auto nalu = previousUnitType8.value();
        units.insert(units.end(), nalu.begin(), nalu.end());
    }
    if (previousUnitType5.has_value()) {
        auto nalu = previousUnitType5.value();
        units.insert(units.end(), nalu.begin(), nalu.end());
    }
    return units;
}
