#pragma once

#include "capture/capture_grab_outcome.h"
#include "common/window_ops.h"

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

    static std::string to_lower_ascii(std::string value);

    /// 枚举窗口 + 调用 ICaptureSource 采集瓦片 + 合成布局；不根据帧内容切换后端。
    static CaptureGrabOutcome grab_process_ui_rgb(DWORD pid,
                                                  const std::vector<window_ops::window_info>& surfaces,
                                                  const ProcessUiCaptureOptions& options,
                                                  ICaptureSource& capture,
                                                  uint64_t now_unix_ms);
};
