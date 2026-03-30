#include "h264_encoder.hpp"
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <string>
#include <cstdlib>
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h> // For htonl on Windows
#else
#include <arpa/inet.h> // For htonl on Linux/Unix
#endif

// Convert Annex-B bytestream into length-prefixed NAL units.
static void annexb_to_length_prefixed(const uint8_t* src, size_t src_size, std::vector<uint8_t>& out) {
    out.clear();
    if (!src || src_size == 0) return;

    auto find_start_code = [&](size_t from, size_t& sc_pos, size_t& sc_len) -> bool {
        for (size_t i = from; i + 2 < src_size; ++i) {
            // 00 00 01
            if (src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 1) {
                sc_pos = i;
                sc_len = 3;
                return true;
            }
            // 00 00 00 01
            if (i + 3 < src_size &&
                src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 0 && src[i + 3] == 1) {
                sc_pos = i;
                sc_len = 4;
                return true;
            }
        }
        return false;
    };

    size_t sc_pos = 0, sc_len = 0;
    if (!find_start_code(0, sc_pos, sc_len)) return;

    while (true) {
        const size_t nalu_start = sc_pos + sc_len;
        if (nalu_start >= src_size) break;

        size_t next_sc_pos = 0, next_sc_len = 0;
        const bool has_next = find_start_code(nalu_start, next_sc_pos, next_sc_len);
        size_t nalu_end = has_next ? next_sc_pos : src_size;

        // Trim trailing zero padding before next start code.
        while (nalu_end > nalu_start && src[nalu_end - 1] == 0) {
            --nalu_end;
        }

        if (nalu_end > nalu_start) {
            const size_t nalu_size = nalu_end - nalu_start;
            uint32_t len_be = htonl(static_cast<uint32_t>(nalu_size));
            out.insert(out.end(),
                       reinterpret_cast<uint8_t*>(&len_be),
                       reinterpret_cast<uint8_t*>(&len_be) + 4);
            out.insert(out.end(), src + nalu_start, src + nalu_end);
        }

        if (!has_next) break;
        sc_pos = next_sc_pos;
        sc_len = next_sc_len;
    }
}

static bool contains_annexb_start_code(const uint8_t* src, size_t src_size) {
    if (!src || src_size < 3) return false;
    for (size_t i = 0; i + 2 < src_size; ++i) {
        // 00 00 01
        if (src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 1) return true;
        // 00 00 00 01
        if (i + 3 < src_size && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 0 && src[i + 3] == 1) return true;
    }
    return false;
}

namespace {
struct EncoderCache {
    SwsContext* sws_ctx = nullptr;
    AVFrame* rgb_frame = nullptr;
    AVFrame* yuv_frame = nullptr;
    // src: provided RGB24 size
    int src_width = 0;
    int src_height = 0;
    // dst: encoder ctx size (fixed)
    int dst_width = 0;
    int dst_height = 0;
};

std::mutex g_cache_mtx;
std::unordered_map<AVCodecContext*, EncoderCache> g_encoder_cache;

void free_encoder_cache(AVCodecContext* ctx) {
    if (!ctx) return;
    auto it = g_encoder_cache.find(ctx);
    if (it == g_encoder_cache.end()) return;
    if (it->second.sws_ctx) sws_freeContext(it->second.sws_ctx);
    if (it->second.rgb_frame) av_frame_free(&it->second.rgb_frame);
    if (it->second.yuv_frame) av_frame_free(&it->second.yuv_frame);
    g_encoder_cache.erase(it);
}

bool ensure_encoder_cache(AVCodecContext* ctx, int srcW, int srcH) {
    if (!ctx || srcW <= 0 || srcH <= 0) return false;
    const int dstW = ctx->width;
    const int dstH = ctx->height;
    if (dstW <= 0 || dstH <= 0) return false;
    auto& cache = g_encoder_cache[ctx];
    if (cache.sws_ctx && cache.rgb_frame && cache.yuv_frame &&
        cache.src_width == srcW && cache.src_height == srcH &&
        cache.dst_width == dstW && cache.dst_height == dstH) {
        return true;
    }

    if (cache.sws_ctx) sws_freeContext(cache.sws_ctx);
    if (cache.rgb_frame) av_frame_free(&cache.rgb_frame);
    if (cache.yuv_frame) av_frame_free(&cache.yuv_frame);
    cache = {};
    cache.src_width = srcW;
    cache.src_height = srcH;
    cache.dst_width = dstW;
    cache.dst_height = dstH;

    cache.sws_ctx = sws_getContext(
        srcW, srcH, AV_PIX_FMT_RGB24,
        dstW, dstH, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!cache.sws_ctx) return false;

    cache.rgb_frame = av_frame_alloc();
    cache.yuv_frame = av_frame_alloc();
    if (!cache.rgb_frame || !cache.yuv_frame) return false;

    cache.rgb_frame->format = AV_PIX_FMT_RGB24;
    cache.rgb_frame->width = srcW;
    cache.rgb_frame->height = srcH;

    cache.yuv_frame->format = AV_PIX_FMT_YUV420P;
    cache.yuv_frame->width = dstW;
    cache.yuv_frame->height = dstH;
    if (av_frame_get_buffer(cache.yuv_frame, 32) < 0) return false;

    return true;
}
std::vector<std::pair<std::string, const AVCodec*>> build_codec_candidates()
{
    auto byName = [](const char* name) -> const AVCodec* { return avcodec_find_encoder_by_name(name); };
    auto byId = []() -> const AVCodec* { return avcodec_find_encoder(AV_CODEC_ID_H264); };
    const char* preferred = std::getenv("RPC_ENCODER_BACKEND"); // auto|sw|nvenc|qsv|amf
    std::string mode = preferred ? preferred : "auto";
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<std::pair<std::string, const AVCodec*>> out;
    auto push = [&](const std::string& name, const AVCodec* c) {
        if (!c) return;
        for (const auto& it : out) {
            if (it.second == c) return;
        }
        out.emplace_back(name, c);
    };

    if (mode == "nvenc") {
        push("nvenc", byName("h264_nvenc"));
    } else if (mode == "qsv") {
        push("qsv", byName("h264_qsv"));
    } else if (mode == "amf") {
        push("amf", byName("h264_amf"));
    } else if (mode == "sw") {
        // Strict software preference: do not fallback to generic "h264" here,
        // because on Windows that commonly maps to h264_mf and reintroduces
        // hardware/MFT-specific instability.
        push("libx264", byName("libx264"));
        push("openh264", byName("libopenh264"));
    } else {
        push("nvenc", byName("h264_nvenc"));
        push("qsv", byName("h264_qsv"));
        push("amf", byName("h264_amf"));
        push("libx264", byName("libx264"));
        push("openh264", byName("libopenh264"));
        push("h264", byId());
    }
    return out;
}
} // namespace

AVCodecContext* create_h264_encoder(int width, int height, int fps) {
    auto candidates = build_codec_candidates();
    if (candidates.empty()) {
        std::cerr << "[encoder] no H264 encoder available" << std::endl;
        return nullptr;
    }
    const int fp = std::max<int>(1, fps);
    for (const auto& [backendName, codec] : candidates) {
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) continue;
        ctx->width = width;
        ctx->height = height;
        ctx->time_base = { 1, fp };
        ctx->framerate = { fp, 1 };
        // Keep GOP reasonably long to avoid frequent bitrate spikes and visible flicker.
        // Too-short GOP (e.g. IDR every few frames) can cause periodic pulsing even on LAN.
        ctx->gop_size = std::max<int>(24, fp);
        ctx->max_b_frames = 0;
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;

        const int64_t px = static_cast<int64_t>(width) * static_cast<int64_t>(height);
        // Be conservative with bitrate to avoid receiver decode drops under jitter.
        int64_t br = px * 5LL;
        br = std::max<int64_t>(1800000LL, std::min<int64_t>(12000000LL, br));
        ctx->bit_rate = static_cast<int>(br);
        ctx->rc_max_rate = static_cast<int>(std::min<int64_t>(24000000LL, br + br / 3));
        ctx->rc_buffer_size = static_cast<int>(std::max<int64_t>(500000LL, br / 2));

        if (ctx->priv_data) {
            if (backendName == "libx264" || backendName == "h264") {
                av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
                av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
                av_opt_set(ctx->priv_data, "profile", "baseline", 0);
                av_opt_set(ctx->priv_data, "x264-params",
                    "repeat-headers=1:scenecut=0:ref=1:aq-mode=1:min-keyint=1:sync-lookahead=0:rc-lookahead=0", 0);
                av_opt_set(ctx->priv_data, "repeat_headers", "1", 0);
            } else if (backendName == "nvenc") {
                av_opt_set(ctx->priv_data, "preset", "p1", 0);
                av_opt_set(ctx->priv_data, "tune", "ll", 0);
                av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
                av_opt_set(ctx->priv_data, "rc", "cbr", 0);
            } else if (backendName == "qsv") {
                av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
                av_opt_set(ctx->priv_data, "look_ahead", "0", 0);
            } else if (backendName == "amf") {
                av_opt_set(ctx->priv_data, "usage", "ultralowlatency", 0);
                av_opt_set(ctx->priv_data, "quality", "speed", 0);
            }
        }

        if (avcodec_open2(ctx, codec, nullptr) == 0) {
            std::cout << "[encoder] selected backend=" << backendName << std::endl;
            return ctx;
        }
        std::cerr << "[encoder] failed backend=" << backendName << ", trying fallback" << std::endl;
        avcodec_free_context(&ctx);
    }
    return nullptr;
}

AVPacket* encode_frame(AVCodecContext* ctx, AVFrame* frame) {
    avcodec_send_frame(ctx, frame);
    AVPacket* pkt = av_packet_alloc();
    if (avcodec_receive_packet(ctx, pkt) == 0)
        return pkt;
    av_packet_free(&pkt);
    return nullptr;
}

// Encode one RGB frame with the provided H264 encoder context.
// rgb_data: packed RGB24 frame buffer (stride = width * 3).
// width, height: input frame size.
// out: encoded H264 access unit output.
// returns true when encoded output is available.
bool encode_rgb(AVCodecContext* ctx, const uint8_t* rgb_data, int width, int height, int64_t& frame_seq,
                std::vector<uint8_t>& out, bool force_keyframe)
{
    if (!ctx || !rgb_data) return false;

    std::scoped_lock lk(g_cache_mtx);
    // Treat width/height as SRC size; sws will scale to ctx->width/height (DST).
    if (!ensure_encoder_cache(ctx, width, height)) return false;
    auto& cache = g_encoder_cache[ctx];

    // 1) Reuse AVFrame/SwsContext to avoid per-frame allocation/initialization.
    AVFrame* frame = cache.rgb_frame;
    AVFrame* yuv_frame = cache.yuv_frame;
    av_image_fill_arrays(frame->data, frame->linesize, rgb_data, AV_PIX_FMT_RGB24, width, height, 1);

    // 2. RGB -> YUV420P
    yuv_frame->pts = frame_seq++;
    // Force IDR only when needed: first frame or explicit request.
    // Let encoder follow gop_size for periodic keyframes to keep cadence stable.
    {
        if (force_keyframe || yuv_frame->pts == 0) {
            yuv_frame->pict_type = AV_PICTURE_TYPE_I;
            yuv_frame->key_frame = 1;
        } else {
            yuv_frame->pict_type = AV_PICTURE_TYPE_NONE;
            yuv_frame->key_frame = 0;
        }
    }
    if (av_frame_make_writable(yuv_frame) < 0) {
        fprintf(stderr, "av_frame_make_writable failed\n");
        return false;
    }

    sws_scale(cache.sws_ctx, frame->data, frame->linesize, 0, height, yuv_frame->data, yuv_frame->linesize);

    // 3) Encode.
    AVPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.data = nullptr;
    pkt.size = 0;

    int ret = avcodec_send_frame(ctx, yuv_frame);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "avcodec_send_frame failed: %s (%d)\n", errbuf, ret);
        return false;
    }
    ret = avcodec_receive_packet(ctx, &pkt);
    if (ret == 0) {
        // Some encoders output Annex-B, some output AVCC (length-prefixed).
        // Detect Annex-B by scanning the whole packet, not only packet head.
        // Some backends may prepend bytes before the first start code.
        out.clear();
        if (pkt.data && pkt.size > 0 && contains_annexb_start_code(pkt.data, static_cast<size_t>(pkt.size))) {
            annexb_to_length_prefixed(pkt.data, pkt.size, out);
        }
        // Keep raw payload for AVCC output or conversion fallback.
        if (out.empty() && pkt.data && pkt.size > 0) {
            out.assign(pkt.data, pkt.data + pkt.size);
        }
        av_packet_unref(&pkt);
    }
    else {
        out.clear();
    }

    return !out.empty();
}

void destroy_h264_encoder(AVCodecContext* ctx)
{
    {
        std::scoped_lock lk(g_cache_mtx);
        free_encoder_cache(ctx);
    }
	if (ctx) {
		avcodec_free_context(&ctx);
        ctx = NULL;
	}

}