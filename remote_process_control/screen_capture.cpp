#include "screen_capture.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
static AVCodecContext* decode_ctx = nullptr;

AVFormatContext* open_screen_capture(const char* source, int width, int height) 
{
   // avdevice_register_all();

    AVInputFormat* ifmt = (AVInputFormat*)av_find_input_format("gdigrab");
    AVFormatContext* fmt_ctx = nullptr;

    AVDictionary* options = nullptr;
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "video_size", "1280x720", 0);

    avformat_open_input(&fmt_ctx, source, ifmt, &options);
    avformat_find_stream_info(fmt_ctx, nullptr);

    AVCodec* decoder = (AVCodec*)avcodec_find_decoder(fmt_ctx->streams[0]->codecpar->codec_id);
    decode_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(decode_ctx, fmt_ctx->streams[0]->codecpar);
    avcodec_open2(decode_ctx, decoder, nullptr);

    return fmt_ctx;
}

AVFrame* decode_frame(AVPacket& pkt) {
    AVFrame* frame = av_frame_alloc();
    if (avcodec_send_packet(decode_ctx, &pkt) < 0) return nullptr;
    if (avcodec_receive_frame(decode_ctx, frame) == 0)
        return frame;
    av_frame_free(&frame);
    return nullptr;
}
