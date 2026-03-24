#include "h264_encoder.hpp"
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h> // For htonl on Windows
#else
#include <arpa/inet.h> // For htonl on Linux/Unix
#endif

// ??????????Annex B ???????????? 00 00 01 / 00 00 00 01??
static void annexb_to_length_prefixed(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out) {
    auto is_start_code_at = [&](size_t pos, size_t& sc_len) -> bool {
        if (pos + 3 >= src_size) return false;
        if (src[pos] == 0 && src[pos + 1] == 0) {
            if (src[pos + 2] == 1) { sc_len = 3; return true; }
            if (pos + 4 < src_size && src[pos + 2] == 0 && src[pos + 3] == 1) { sc_len = 4; return true; }
        }
        return false;
    };

    size_t i = 0;
    while (i + 3 < src_size) {
        // ????????
        size_t start = i;
        size_t sc_len = 0;
        while (start + 3 < src_size && !is_start_code_at(start, sc_len)) {
            ++start;
        }
        if (start + 3 >= src_size) break;

        const size_t nalu_start = start + sc_len;
        // ????????????
        size_t nalu_end = nalu_start;
        size_t next_sc_len = 0;
        while (nalu_end + 3 < src_size && !is_start_code_at(nalu_end, next_sc_len)) {
            ++nalu_end;
        }
        if (nalu_end > src_size) nalu_end = src_size;

        const size_t nalu_size = (nalu_end > nalu_start) ? (nalu_end - nalu_start) : 0;
        if (nalu_size > 0) {
            uint32_t len_be = htonl(static_cast<uint32_t>(nalu_size));
            out.insert(out.end(), reinterpret_cast<uint8_t*>(&len_be), reinterpret_cast<uint8_t*>(&len_be) + 4);
            out.insert(out.end(), src + nalu_start, src + nalu_start + nalu_size);
        }

        i = nalu_end;
    }
}

AVCodecContext* create_h264_encoder(int width, int height, int fps) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width = width;
    ctx->height = height;
    const int fp = std::max<int>(1, fps);
    ctx->time_base = { 1, fp };
    ctx->framerate = { fp, 1 };
    // ?? GOP = ????? IDR???????????????????????? fp*2 ?? 30fps ??? 60??? 2s ???????
    ctx->gop_size = std::min<int>(30, std::max<int>(5, fp / 2));
    ctx->max_b_frames = 0;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    // ???????/?????????????????????????? 3?C20 Mbps??
    const int64_t px = static_cast<int64_t>(width) * static_cast<int64_t>(height);
    int64_t br = px * 7LL;
    br = std::max<int64_t>(2500000LL, std::min<int64_t>(20000000LL, br));
    ctx->bit_rate = static_cast<int>(br);
    ctx->rc_max_rate = static_cast<int>(std::min<int64_t>(24000000LL, br + br / 4));
    ctx->rc_buffer_size = static_cast<int>(br);

    // ????? + ???????preset ??????????????aq-mode ????????/????
    if (ctx->priv_data) {
        av_opt_set(ctx->priv_data, "preset", "fast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->priv_data, "profile", "baseline", 0);
        av_opt_set(ctx->priv_data, "x264-params",
            "repeat-headers=1:scenecut=0:ref=2:aq-mode=1:min-keyint=1:sync-lookahead=0:rc-lookahead=0", 0);
        av_opt_set(ctx->priv_data, "repeat_headers", "1", 0);
    }

    avcodec_open2(ctx, codec, nullptr);
    return ctx;
}

AVPacket* encode_frame(AVCodecContext* ctx, AVFrame* frame) {
    avcodec_send_frame(ctx, frame);
    AVPacket* pkt = av_packet_alloc();
    if (avcodec_receive_packet(ctx, pkt) == 0)
        return pkt;
    av_packet_free(&pkt);
    return nullptr;
}

// ???? m_av_codec_ctx ????????? H264 ??????
// rgb_data: ????RGB???????????? width*3
// width, height: ?????
// out: ???H264????
// ???? true ?????????
bool encode_rgb(AVCodecContext* ctx, const uint8_t* rgb_data, int width, int height, int64_t& frame_seq,
                std::vector<uint8_t>& out, bool force_keyframe)
{
    if (!ctx || !rgb_data) return false;

    // 1. ????AVFrame?????RGB????
    AVFrame* frame = av_frame_alloc();
    if (!frame) return false;
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = width;
    frame->height = height;
    av_image_fill_arrays(frame->data, frame->linesize, rgb_data, AV_PIX_FMT_RGB24, width, height, 1);
    
    
    // 2. ????YUV420P
    AVFrame* yuv_frame = av_frame_alloc();
    yuv_frame->format = AV_PIX_FMT_YUV420P;
    yuv_frame->width = width;
    yuv_frame->height = height;
    yuv_frame->pts = frame_seq++;
    // IDR��GOP �߽硢��֡����ֱ��ʱ仯����֡��force_keyframe�������� WebRTC �� SPS ����� P ֡��������
    {
        const int gop = std::max<int>(1, ctx->gop_size);
        if (force_keyframe || yuv_frame->pts == 0 || (yuv_frame->pts % gop) == 0) {
            yuv_frame->pict_type = AV_PICTURE_TYPE_I;
            yuv_frame->key_frame = 1;
        }
    }
    if (av_frame_get_buffer(yuv_frame, 32) < 0) {
        fprintf(stderr, "av_frame_get_buffer failed\n");
        av_frame_free(&frame);
        av_frame_free(&yuv_frame);
        return false;
    }
    if (av_frame_make_writable(yuv_frame) < 0) {
        fprintf(stderr, "av_frame_make_writable failed\n");
        av_frame_free(&frame);
        av_frame_free(&yuv_frame);
        return false;
    }

    struct SwsContext* sws_ctx = sws_getContext(
        width, height, AV_PIX_FMT_RGB24,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BICUBIC, nullptr, nullptr, nullptr);

    sws_scale(sws_ctx, frame->data, frame->linesize, 0, height, yuv_frame->data, yuv_frame->linesize);

    // 3. ????
    AVPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.data = nullptr;
    pkt.size = 0;

    int ret = avcodec_send_frame(ctx, yuv_frame);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "avcodec_send_frame failed: %s (%d)\n", errbuf, ret);

        sws_freeContext(sws_ctx);
        av_frame_free(&frame);
        av_frame_free(&yuv_frame);
        return false;
    }
    ret = avcodec_receive_packet(ctx, &pkt);
    if (ret == 0) {
        // ?????????????
        out.clear();
        annexb_to_length_prefixed(pkt.data, pkt.size, out);
        // Fallback?????????????????? AnnexB????? start code???????????????????????
        if (out.empty() && pkt.data && pkt.size > 0) {
            out.assign(pkt.data, pkt.data + pkt.size);
        }
        av_packet_unref(&pkt);
    }
    else {
        out.clear();
    }

    // 4. ??????
    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    av_frame_free(&yuv_frame);

    return !out.empty();
}

void destroy_h264_encoder(AVCodecContext* ctx)
{
	if (ctx) {
		avcodec_free_context(&ctx);
        ctx = NULL;
	}

}