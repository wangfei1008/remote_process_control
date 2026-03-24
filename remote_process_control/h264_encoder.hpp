#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h> 
#include <libswscale/swscale.h>
}
#include <cstdint>
#include <vector>

AVCodecContext* create_h264_encoder(int width, int height, int fps);
AVPacket* encode_frame(AVCodecContext* ctx, AVFrame* frame);
/** frame_seq must increase monotonically per encoder; reset to 0 when recreating AVCodecContext to avoid PTS mismatch and decoder stalls. */
bool encode_rgb(AVCodecContext* ctx, const uint8_t* rgb_data, int width, int height, int64_t& frame_seq,
                std::vector<uint8_t>& out, bool force_keyframe = false);
void destroy_h264_encoder(AVCodecContext* ctx);
