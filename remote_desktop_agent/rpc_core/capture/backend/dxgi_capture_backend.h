#pragma once

// ============================================================
// dxgi_capture_backend.h
// DXGI Desktop Duplication 采集后端
// ============================================================

#include "i_capture_backend.h"
#include <memory>

namespace capture {

class DxgiCaptureBackend final : public ICaptureBackend {
public:
    // 探测当前环境是否支持 DXGI Desktop Duplication
    static bool probe();

    DxgiCaptureBackend();
    ~DxgiCaptureBackend() override;

    bool capture_tiles(std::span<const win32::WindowInfo> surfaces,
                       std::vector<WindowTile>&           out_tiles,
                       uint64_t                           now_unix_ms) override;

    void on_new_session() override;

    CaptureBackendKind kind() const override { return CaptureBackendKind::Dxgi; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace capture
