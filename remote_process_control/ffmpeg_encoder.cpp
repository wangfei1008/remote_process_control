// ffmpeg_encoder.cpp
#include "ffmpeg_encoder.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#include <iostream>
#include <algorithm>
#include <cstdint>

FFmpegEncoder::FFmpegEncoder(int width, int height, int fps) : width(width), height(height) {
    //avcodec_register_all();
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    codecCtx = avcodec_alloc_context3(codec);
    {
        const int64_t px = static_cast<int64_t>(width) * static_cast<int64_t>(height);
        int64_t br = px * 7LL;
        br = std::max<int64_t>(2500000LL, std::min<int64_t>(20000000LL, br));
        codecCtx->bit_rate = static_cast<int>(br);
        codecCtx->rc_max_rate = static_cast<int>(std::min<int64_t>(24000000LL, br + br / 4));
        codecCtx->rc_buffer_size = static_cast<int>(br);
    }
    codecCtx->width = width;
    codecCtx->height = height;
    codecCtx->time_base = AVRational{ 1, fps };
    codecCtx->framerate = AVRational{ fps, 1 };
    codecCtx->gop_size = 10;
    codecCtx->max_b_frames = 1;
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    av_opt_set(codecCtx->priv_data, "preset", "fast", 0);
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        std::cerr << "Could not open codec" << std::endl;
        return;
    }

    frame = av_frame_alloc();
    frame->format = codecCtx->pix_fmt;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 32);

    pkt = av_packet_alloc();
}

FFmpegEncoder::~FFmpegEncoder() {
    avcodec_free_context(&codecCtx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
}

bool FFmpegEncoder::encode_frame(const uint8_t* rgbData, int size) {
    struct SwsContext* swsCtx = sws_getContext(
        width, height, AV_PIX_FMT_BGR24,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!swsCtx) {
        std::cerr << "Failed to init sws context" << std::endl;
        return false;
    }

    const uint8_t* srcSlice[] = { rgbData };
    int srcStride[] = { 3 * width };

    sws_scale(swsCtx, srcSlice, srcStride, 0, height, frame->data, frame->linesize);

    frame->pts = frameCounter++;

    int ret = avcodec_send_frame(codecCtx, frame);
    if (ret < 0) {
        std::cerr << "Error sending frame" << std::endl;
        sws_freeContext(swsCtx);
        return false;
    }

    ret = avcodec_receive_packet(codecCtx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        sws_freeContext(swsCtx);
        return false;
    }
    else if (ret < 0) {
        std::cerr << "Error encoding frame" << std::endl;
        sws_freeContext(swsCtx);
        return false;
    }

    sws_freeContext(swsCtx);
    return true;
}

AVPacket* FFmpegEncoder::get_packet() {
    return pkt;
}

void FFmpegEncoder::free_packet() {
    av_packet_unref(pkt);
}

