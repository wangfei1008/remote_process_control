#pragma once

#include <windows.h>

#include <vector>

#include "capture/gdi_capture.h"

class WindowFrameGrabber {
public:
    static std::vector<uint8_t> capture_main_window_image(GdiCapture& gdi_capture,
                                                          HWND hwnd,
                                                          int& out_width,
                                                          int& out_height,
                                                          int& out_min_left,
                                                          int& out_min_top,
                                                          bool include_non_client);
};

