#pragma once

#include "capture/i_capture_source.h"

class GdiProcessUiCaptureBackend final : public ICaptureSource {
public:
    static bool probe();

    GdiProcessUiCaptureBackend();
    ~GdiProcessUiCaptureBackend() override;

    bool capture_tiles(const std::vector<window_ops::window_info>& surfaces,
                       std::vector<ProcessUiWindowTile>& tiles,
                       uint64_t now_unix_ms) override;

    void reset_session_recovery() override;

    bool uses_hw_capture() const override { return false; }
    const char* backend_name() const override { return "gdi"; }
};
