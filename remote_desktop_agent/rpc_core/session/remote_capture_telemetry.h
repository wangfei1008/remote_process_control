#pragma once

#include <cstdint>

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
    //本 session 是否已禁用 DXGI（例如检测到不稳定后强制切回软件采集）。
    bool dxgi_disabled_for_session = false;
    //检测到“顶部黑条”等异常画面的连续次数/连击计数（用于判断采集异常趋势）。
    int top_black_strip_streak = 0;
    //DXGI 不稳定评分/指标（越高表示越不稳定，用于策略决策与诊断展示）。
    int dxgi_instability_score = 0;
};

