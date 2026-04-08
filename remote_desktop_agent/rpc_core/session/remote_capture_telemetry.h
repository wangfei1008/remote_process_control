#pragma once

#include <cstdint>

// 视频采集/编码遥测快照（用于 frameMark / captureHealth）。
// 目前为阶段性结构；后续会把真实采集与编码数据填充完整。
struct remote_capture_telemetry {
    int capture_width = 0;
    int capture_height = 0;

    uint32_t last_capture_ms = 0;
    uint32_t last_encode_ms = 0;
    uint64_t last_frame_unix_ms = 0;

    bool last_capture_used_hw = false;
    bool dxgi_disabled_for_session = false;
    int top_black_strip_streak = 0;
    int dxgi_instability_score = 0;
    uint64_t force_software_capture_until_unix_ms = 0;
};

