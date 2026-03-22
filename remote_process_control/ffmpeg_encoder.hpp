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
    bool encodeFrame(const uint8_t* rgbData, int size);
    AVPacket* getPacket();
    void freePacket();

private:
    AVCodecContext* codecCtx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int frameCounter = 0;
    int width;
    int height;
};
