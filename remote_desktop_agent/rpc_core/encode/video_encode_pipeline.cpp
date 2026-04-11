#include "encode/video_encode_pipeline.h"

#include "encode/h264_avcc_utils.h"
#include "common/rpc_time.h"

#include <algorithm>
#include <iostream>

VideoEncodePipeline::~VideoEncodePipeline()
{
    destroy_encoder();
}

void VideoEncodePipeline::configure(int fps, int layout_change_threshold_px, int layout_change_required_streak)
{
    m_encode_fps = (std::max)(1, fps);
    m_encoder_layout_policy.configure(layout_change_threshold_px, layout_change_required_streak);
}

bool VideoEncodePipeline::initialize_encoder(int width, int height)
{
    destroy_encoder();
    if (width <= 0 || height <= 0) return false;
    m_av_codec_ctx = create_h264_encoder(width, height, m_encode_fps);
    m_extradata_spspps = parse_avcc_spspps(m_av_codec_ctx);
    m_encode_frame_seq = 0;
    return (m_av_codec_ctx != nullptr);
}

void VideoEncodePipeline::reset_for_stream_start()
{
    m_pending_force_keyframe.store(false);
    m_last_force_keyframe_unix_ms.store(0, std::memory_order_relaxed);
    m_encode_frame_seq = 0;
    m_encoder_layout_policy.reset();
}

void VideoEncodePipeline::request_force_keyframe_with_cooldown(uint64_t now_ms)
{
    constexpr uint64_t k_cooldown_ms = 3000;
    while (true) {
        const uint64_t last_allowed_ms = m_last_force_keyframe_unix_ms.load(std::memory_order_relaxed);
        if (last_allowed_ms != 0 && now_ms >= last_allowed_ms && (now_ms - last_allowed_ms) < k_cooldown_ms) {
            return;
        }

        uint64_t expected = last_allowed_ms;
        if (m_last_force_keyframe_unix_ms.compare_exchange_weak(
                expected, now_ms, std::memory_order_relaxed, std::memory_order_relaxed)) {
            m_pending_force_keyframe.store(true, std::memory_order_relaxed);
            return;
        }
    }
}

bool VideoEncodePipeline::ensure_encoder_layout(int captured_w, int captured_h, bool had_successful_video,
                                                int& io_target_w, int& io_target_h, bool& applied_layout_out)
{
    applied_layout_out = false;
    if (!m_av_codec_ctx) {
        io_target_w = captured_w;
        io_target_h = captured_h;
        return initialize_encoder(io_target_w, io_target_h);
    }

    // io_target_w/h 必须由「编码器真实宽高」驱动策略；若每帧都传采集宽高，captured 与 current 永远相等，分辨率永远无法切换。
    const int encoder_w = m_av_codec_ctx->width;
    const int encoder_h = m_av_codec_ctx->height;

    if (!m_encoder_layout_policy.should_apply_layout_change(captured_w, captured_h, encoder_w, encoder_h, had_successful_video)) {
        io_target_w = encoder_w;
        io_target_h = encoder_h;

        return true;
    }

    std::cout << "[encode] keep encoder layout " << encoder_w << "x" << encoder_h << " (captured "
                << captured_w << "x" << captured_h << ", successful_video=" << had_successful_video << ")\n";

    io_target_w = captured_w;
    io_target_h = captured_h;
    applied_layout_out = true;
    return initialize_encoder(io_target_w, io_target_h);
}

VideoEncodeResult VideoEncodePipeline::encode_frame(const std::vector<uint8_t>& frame,
                                                    int captured_w,
                                                    int captured_h,
                                                    bool applied_layout,
                                                    std::chrono::steady_clock::time_point t_cap_begin,
                                                    std::chrono::steady_clock::time_point t_after_cap)
{
    VideoEncodeResult result;
    if (!m_av_codec_ctx || frame.empty() || captured_w <= 0 || captured_h <= 0) return result;

    std::vector<uint8_t> h264_data;
    const auto t_enc_begin = std::chrono::steady_clock::now();
    const bool force_keyframe = applied_layout || m_pending_force_keyframe.load();
    const bool ok = encode_rgb(m_av_codec_ctx, frame.data(), captured_w, captured_h, m_encode_frame_seq, h264_data, force_keyframe);
    const auto t_enc_end = std::chrono::steady_clock::now();
    if (!ok || h264_data.empty()) return result;

    if (!validate_h264_avcc_payload(h264_data.data(), h264_data.size())) {
        static auto s_last_invalid_log = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last_invalid_log > std::chrono::seconds(1)) {
            s_last_invalid_log = now;
            std::cout << "[encode] invalid h264 avcc payload, drop frame\n";
        }
        result.invalid_payload = true;
        return result;
    }

    const std::byte* p = reinterpret_cast<const std::byte*>(h264_data.data());
    result.sample.assign(p, p + h264_data.size());

    bool has_idr = false;
    bool has_sps = false;
    bool has_pps = false;
    inspect_h264_avcc_sample(result.sample, has_idr, has_sps, has_pps);
    if (has_idr && (!has_sps || !has_pps) && m_extradata_spspps.has_value()) {
        const auto& spspps = m_extradata_spspps.value();
        rtc::binary patched;
        patched.reserve(spspps.size() + result.sample.size());
        patched.insert(patched.end(), spspps.begin(), spspps.end());
        patched.insert(patched.end(), result.sample.begin(), result.sample.end());
        result.sample.swap(patched);
    }

    result.capture_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t_after_cap - t_cap_begin).count());
    result.encode_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t_enc_end - t_enc_begin).count());
    result.frame_unix_ms = rpc_unix_epoch_ms();
    result.encode_ok = true;
    m_pending_force_keyframe.store(false);
    return result;
}

void VideoEncodePipeline::destroy_encoder()
{
    if (m_av_codec_ctx) {
        destroy_h264_encoder(m_av_codec_ctx);
        m_av_codec_ctx = nullptr;
    }
    m_extradata_spspps = std::nullopt;
    m_encode_frame_seq = 0;
}

