#pragma once

#include <cstdint>
#include <vector>
#include <mutex>
#include <optional>
#include <condition_variable>
#include <deque>

#include "rtc/rtc.hpp"

struct CaptureGrabOutcome {
    bool ok = true;
    bool need_hold_on_empty_fallback = false;
    bool used_hw_capture = false;
    int width = 0;
    int height = 0;
    int cap_min_left = 0;
    int cap_min_top = 0;
    std::vector<uint8_t> frame;
};

struct CapturedFrame {
    uint64_t frame_id = 0;
    // Capture-complete absolute unix ms (epoch ms).
    uint64_t unix_ms = 0;
    // Unix epoch ms immediately before this frame's grab (scheduling / pre-capture).
    uint64_t prep_unix_ms = 0;
    CaptureGrabOutcome grab_outcome;
};

struct LatestFrameSlot {
    std::mutex mtx;
    std::condition_variable cv;
    std::optional<CapturedFrame> latest;
    uint64_t dropped_by_overwrite = 0;
    uint64_t stored_frames = 0;
};

struct EncodedSample {
    //帧序号
    uint64_t frame_id = 0;
    //编码完成时刻的绝对时间戳
    uint64_t unix_ms = 0;
    //采集完成时刻的绝对时间戳
    uint64_t cap_unix_ms = 0;
    //编码后的码流数据
    rtc::binary sample;
    //这帧采集是否走硬件路径(DXGI)，供遥测使用
    bool used_hw_capture = false;

    uint64_t prep_unix_ms;

    int w = 0;
    int h = 0;
};

struct LatestEncodedQueue {
    std::mutex mtx;
    std::deque<EncodedSample> q;
    size_t capacity = 1;
    uint64_t dropped_by_overflow = 0;
    uint64_t pushed = 0;
};

// 视频采集/编码遥测快照（用于 frameMark / captureHealth）。
// 目前为阶段性结构；后续会把真实采集与编码数据填充完整。
struct remote_capture_telemetry {
    int capture_width = 0;
    int capture_height = 0;
    //该帧的“帧时间戳”兜底字段（Unix epoch 毫秒）。当没有更精确的 capture/encode 时间戳时用它补齐；也用于无新帧时的遥测回填兜底
    uint64_t last_frame_unix_ms = 0;

    // Absolute unix-ms timestamps (epoch ms) for the current encoded frame.
    //采集完成时刻的绝对时间戳
    uint64_t last_capture_unix_ms = 0;
    //编码完成时刻的绝对时间戳
    uint64_t last_encode_unix_ms = 0;
    //采集前准备时刻（开始 grab 之前那一瞬）的绝对时间戳（Unix epoch 毫秒，timestamp）。用于把“prep→capture”这段也纳入 SEI 延迟链路（发送端若为 0 会回退成 capMs）
    uint64_t last_prep_unix_ms = 0;

    //上一帧/当前帧采集是否走硬件路径（例如 DXGI）而非软件（例如 GDI）
    bool last_capture_used_hw = false;
};