#pragma once

// ============================================================
// capture_pipeline.h
// 采集管线：组合 resolver + backend + composer + filter
//
// 这是原 ProcessUiCapture 的替代，但职责更清晰：
//   - 不再是一堆 static 方法的工具类
//   - 依赖全部通过构造注入，便于测试
//   - grab_raw_frame 是唯一对外的主接口
// ============================================================

#include "../infra/win32_window.h"
#include "../backend/i_capture_backend.h"
#include "frame_composer.h"
#include "frame_filter.h"

#include "common/remote_video_contract.h"

#include <cstdint>
#include <vector>

namespace capture {

// ----------------------------------------------------------
// GrabResult：单次采集的完整输出
// ----------------------------------------------------------
struct GrabResult {
    bool ok = false;

    // 合成后的 RGB24 帧
    ComposedFrame frame;

    // 遥测
    struct Telemetry {
        uint64_t frame_id     = 0;
        uint64_t now_unix_ms  = 0;
        uint64_t prep_unix_ms = 0;
        uint64_t cap_unix_ms  = 0;
        CaptureBackendKind backend = CaptureBackendKind::Unknown;
    } telem;
};

// ----------------------------------------------------------
// CapturePipeline
// ----------------------------------------------------------
class CapturePipeline {
public:
    struct Deps {
        ICaptureBackend& backend;
        const FrameComposer& composer;
        const FrameFilter&   filter;
    };

    explicit CapturePipeline(Deps deps);

    // 主接口：采集 + 合成 + 过滤，填充 RawFrame 合约
    // surfaces  : 已由 CaptureTargetResolver 确定的窗口列表
    // filter_black : 是否启用可疑帧过滤（对应 RPC_FILTER_CAPTURE_BLACK_FRAMES）
    bool grab_raw_frame(
        std::span<const win32::WindowInfo>    surfaces,
        uint64_t                              now_unix_ms,
        uint64_t                              prep_unix_ms,
        uint64_t                              frame_id,
        bool                                  filter_black,
        rpc_video_contract::RawFrame&         out_frame,
        rpc_video_contract::TelemetrySnapshot& out_telem) const;

    // 仅合成（不填 RawFrame 合约，用于调试或中间层）
    GrabResult grab(std::span<const win32::WindowInfo> surfaces,
                    uint64_t                           now_unix_ms,
                    const FrameComposer::Options&      opts,
                    bool                               filter_black) const;

private:
    Deps m_deps;
};

} // namespace capture
