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
//frame_seq 在每个编码器实例内必须单调递增；重建 AVCodecContext 时应重置为 0，避免 PTS 不匹配和解码卡顿。
bool encode_rgb(AVCodecContext* ctx, const uint8_t* rgb_data, int width, int height, int64_t& frame_seq,
                std::vector<uint8_t>& out, bool force_keyframe = false);
void destroy_h264_encoder(AVCodecContext* ctx);
