#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <optional>
#include <vector>
#include <chrono>

#include "rtc/rtc.hpp"
#include "encode/h264_encoder.hpp"
#include "encode/encoder_layout_policy.h"

struct VideoEncodeResult {
    bool encode_ok = false;
    bool invalid_payload = false;
    rtc::binary sample;
    uint64_t frame_unix_ms = 0;
};

class VideoEncodePipeline {
public:
    ~VideoEncodePipeline();
    void configure(int fps, int layout_change_threshold_px, int layout_change_required_streak);
    bool initialize_encoder(int width, int height);
    void reset_for_stream_start();
    void request_force_keyframe_with_cooldown(uint64_t now_ms);
    bool ensure_encoder_layout(int captured_w,
                               int captured_h,
                               bool had_successful_video,
                               int& io_target_w,
                               int& io_target_h,
                               bool& applied_layout_out);

    VideoEncodeResult encode_frame(const std::vector<uint8_t>& frame,
                                   int captured_w,
                                   int captured_h,
                                   bool applied_layout);

private:
    void destroy_encoder();

    AVCodecContext* m_av_codec_ctx = nullptr;
    std::optional<std::vector<std::byte>> m_extradata_spspps = std::nullopt;
    EncoderLayoutPolicy m_encoder_layout_policy;
    int m_encode_fps = 30;
    int64_t m_encode_frame_seq = 0;

    std::atomic<bool> m_pending_force_keyframe{false};
    std::atomic<uint64_t> m_last_force_keyframe_unix_ms{0};
};

