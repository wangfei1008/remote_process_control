#pragma once

#include "common/remote_video_contract.h"
#include "common/window_ops.h"
#include "common/character_conversion.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

class ICaptureSource;

enum class ProcessUiCompositeLayout {
    Bbox,
    Horizontal,
    Vertical,
    Grid,
};


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


struct ProcessUiCaptureOptions {
    ProcessUiCompositeLayout composite_layout = ProcessUiCompositeLayout::Bbox;
    int composite_padding_px = 8;
    int composite_grid_columns = 2;
};

class ProcessUiCapture {
public:
    // Debug: with RPC_LOG_PROCESS_SURFACES=1 in rpc_config.ini, logs surfaces, union rect, capture sizes, cap_rect,
    // per-window visibility/style/LWA/DWM cloaked/display affinity, and English heuristic line (why=).

    static ProcessUiCaptureOptions load_layout_options_from_config();

    /// 枚举窗口 + 调用 ICaptureSource 采集瓦片 + 合成布局；不根据帧内容切换后端。
    static CaptureGrabOutcome grab_process_ui_rgb(DWORD pid,
                                                  const std::vector<window_ops::window_info>& surfaces,
                                                  const ProcessUiCaptureOptions& options,
                                                  ICaptureSource& capture,
                                                  uint64_t now_unix_ms);

    /// Contract version: capture + compose into RawFrame (CPU/RGB24), plus telemetry snapshot.
    /// - prep_unix_ms: unix epoch ms immediately before grabbing tiles
    /// - out_telem timestamps are unix epoch ms
    static bool grab_process_ui_raw_frame(DWORD pid,
                                          const std::vector<window_ops::window_info>& surfaces,
                                          const ProcessUiCaptureOptions& options,
                                          ICaptureSource& capture,
                                          uint64_t now_unix_ms,
                                          uint64_t prep_unix_ms,
                                          uint64_t frame_id,
                                          rpc_video_contract::RawFrame& out_frame,
                                          rpc_video_contract::TelemetrySnapshot& out_telem);
};
