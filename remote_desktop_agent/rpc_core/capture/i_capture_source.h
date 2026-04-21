#pragma once

#include "common/window_ops.h"
#include "capture/process_ui_tile.h"

#include <cstdint>
#include <vector>

/// 进程级固定一种实现：负责按 surfaces 产出瓦片；不切换 DXGI/GDI，失败仅内部 reset/retry。
class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    virtual bool init(); // 默认 true
    virtual void shutdown();

    /// 一次整帧：所有 HWND 瓦片须成功，否则返回 false（与原先 DXGI 批语义一致）。
    virtual bool capture_tiles(const std::vector<window_ops::window_info>& surfaces,
                               std::vector<ProcessUiWindowTile>& tiles,
                               uint64_t now_unix_ms) = 0;

    /// 新视频流开始或 DXGI 连续失败后的同后端恢复（不更换实现类）。
    virtual void reset_session_recovery();

    virtual bool uses_hw_capture() const = 0;
    virtual const char* backend_name() const = 0;
};
