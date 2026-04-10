#pragma once

#include "capture/capture_grab_outcome.h"

#include <windows.h>

#include <cstdint>
#include <string>

class GdiCapture;
class DXGICapture;
class CaptureBackendState;

enum class ProcessUiSessionBackendMode {
    Gdi,
    Dxgi,
};

enum class ProcessUiCompositeLayout {
    Bbox,
    Horizontal,
    Vertical,
    Grid,
};

struct ProcessUiCaptureOptions {
    ProcessUiSessionBackendMode session_backend = ProcessUiSessionBackendMode::Gdi;
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

    // Captures all visible top-level HWNDs for PID; drops whole frame if any tile fails or DXGI batch fails.
    static CaptureGrabOutcome grab_process_ui_rgb(DWORD pid,
                                                    const ProcessUiCaptureOptions& options,
                                                    GdiCapture& gdi_capture,
                                                    DXGICapture& dxgi_capture,
                                                    CaptureBackendState& capture_backend_state,
                                                    uint64_t now_unix_ms);
};
