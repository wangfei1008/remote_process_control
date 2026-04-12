#pragma once

#include "capture/i_capture_source.h"

#include <memory>

class DxgiProcessUiCaptureBackend final : public ICaptureSource {
public:
    static bool probe();

    DxgiProcessUiCaptureBackend();
    ~DxgiProcessUiCaptureBackend() override;

    bool capture_tiles(const std::vector<ProcessSurfaceInfo>& surfaces,
                       std::vector<ProcessUiWindowTile>& tiles,
                       uint64_t now_unix_ms) override;

    void reset_session_recovery() override;

    bool uses_hw_capture() const override { return true; }
    const char* backend_name() const override { return "dxgi"; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
