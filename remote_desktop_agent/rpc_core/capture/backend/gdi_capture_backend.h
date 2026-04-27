#pragma once

// ============================================================
// gdi_capture_backend.h
// GDI 采集后端（fallback）
// ============================================================

#include "i_capture_backend.h"

namespace capture {

class GdiCaptureBackend final : public ICaptureBackend {
public:
    static bool probe() { return true; }  // GDI 始终可用

    GdiCaptureBackend()  = default;
    ~GdiCaptureBackend() = default;

    bool capture_tiles(std::span<const win32::WindowInfo> surfaces,
                       std::vector<WindowTile>&           out_tiles,
                       uint64_t                           now_unix_ms) override;

    // GDI 无跨流状态，on_new_session 为空
    CaptureBackendKind kind() const override { return CaptureBackendKind::Gdi; }
};

} // namespace capture
