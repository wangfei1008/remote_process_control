#pragma once
#include <string>
#include <vector>
#include "stream.h"
#include "desktop_capture.hpp"
#include "h264_encoder.hpp"

class DesktopScreenSource : public StreamSource {
    std::optional<std::vector<std::byte>> previousUnitType5 = std::nullopt;
    std::optional<std::vector<std::byte>> previousUnitType7 = std::nullopt;
    std::optional<std::vector<std::byte>> previousUnitType8 = std::nullopt;
public:
    DesktopScreenSource(int width, int height, int fps, bool loop = false);

    void start() override;
    void stop() override;
    void load_next_sample() override;
    rtc::binary get_sample() override;
    uint64_t get_sample_time_us() override;
    uint64_t get_sample_duration_us() override;

    std::vector<std::byte> initial_nalus();
private:
	AVCodecContext* m_av_codec_ctx = nullptr;
    DesktopCapture capture;
    rtc::binary sample;
    uint64_t sampleTime_us = 0;
    int64_t m_encode_frame_seq = 0;
    int fps;
    bool running = false;
    bool loop;
	int m_width;
	int m_height;
	int m_fps;
};

