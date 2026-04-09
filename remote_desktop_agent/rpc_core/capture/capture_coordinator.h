#pragma once

#include <windows.h>

#include <vector>

#include "capture/capture_backend_state.h"
#include "capture/dxgi_capture.h"
#include "capture/gdi_capture.h"

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

class CaptureCoordinator {
public:
    static CaptureGrabOutcome grab_rgb_frame(bool use_hw_capture,
                                             bool capture_all_windows,
                                             bool lock_capture_backend,
                                             DWORD capture_pid,
                                             HWND main_window,
                                             GdiCapture& gdi_capture,
                                             DXGICapture& dxgi_capture,
                                             CaptureBackendState& capture_backend_state);
};

