#pragma once

// ============================================================
// i_capture_backend.h
// 采集后端接口
//
// 相比原 ICaptureSource 的改动：
//   1. 删除 init() / shutdown()：构造/析构负责，调用时机不再模糊
//   2. reset_session_recovery() → on_new_session()：名字说清楚时机
//   3. 参数改用 std::span：不要求调用方构造 vector
// ============================================================

#include "../infra/win32_window.h"
#include "window_tile.h"
#include "capture_backend_kind.h"

#include <cstdint>
#include <span>
#include <vector>

namespace capture {

class ICaptureBackend {
public:
    virtual ~ICaptureBackend() = default;

    // 采集一批窗口的 RGB24 瓦片
    // 全部成功 → 返回 true，tiles 填满
    // 任意失败 → 返回 false，tiles 内容不可信
    [[nodiscard]]
    virtual bool capture_tiles(
        std::span<const win32::WindowInfo> surfaces,
        std::vector<WindowTile>&           out_tiles,
        uint64_t                           now_unix_ms) = 0;

    // 新视频流开始时由 CaptureSession 调用
    // 用途：清除跨流的失败计数器、重置 DXGI duplication 等
    virtual void on_new_session() {}

    virtual CaptureBackendKind kind() const = 0;
};

} // namespace capture
