// ffmpeg_encoder.hpp
#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

class FFmpegEncoder {
public:
    FFmpegEncoder(int width, int height, int fps);
    ~FFmpegEncoder();
    bool encode_frame(const uint8_t* rgbData, int size);
    AVPacket* get_packet();
    void free_packet();

private:
    AVCodecContext* codecCtx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int frameCounter = 0;
    int width;
    int height;
};
