#include "capture/window_frame_grabber.h"
#include "common/window_rect_utils.h"

#include <algorithm>
#include <cstring>

std::vector<uint8_t> WindowFrameGrabber::capture_main_window_image(GdiCapture& gdi_capture,
                                                                   HWND hwnd,
                                                                   int& out_width,
                                                                   int& out_height,
                                                                   int& out_min_left,
                                                                   int& out_min_top,
                                                                   bool include_non_client)
{
    out_width = 0;
    out_height = 0;
    out_min_left = 0;
    out_min_top = 0;
    if (!hwnd || !IsWindow(hwnd)) return {};

    RECT capture_rect{};
    if (include_non_client) {
        if (!window_rect_utils::get_effective_window_rect(hwnd, capture_rect)) return {};
    } else {
        RECT client_rect{};
        if (!GetClientRect(hwnd, &client_rect)) return {};
        POINT top_left{client_rect.left, client_rect.top};
        POINT bottom_right{client_rect.right, client_rect.bottom};
        if (!ClientToScreen(hwnd, &top_left) || !ClientToScreen(hwnd, &bottom_right)) return {};
        capture_rect.left = top_left.x;
        capture_rect.top = top_left.y;
        capture_rect.right = bottom_right.x;
        capture_rect.bottom = bottom_right.y;
    }

    const int width = capture_rect.right - capture_rect.left;
    const int height = capture_rect.bottom - capture_rect.top;
    if (width <= 0 || height <= 0) return {};

    auto frame = gdi_capture.capture(hwnd, width, height, include_non_client);
    if (frame.empty()) return {};

    out_width = width & ~1;
    out_height = height & ~1;
    out_min_left = capture_rect.left;
    out_min_top = capture_rect.top;
    if (out_width <= 0 || out_height <= 0) return {};

    if (out_width != width || out_height != height) {
        std::vector<uint8_t> cropped(static_cast<size_t>(out_width) * static_cast<size_t>(out_height) * 3u, 0);
        for (int y = 0; y < out_height; ++y) {
            const size_t src = static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
            const size_t dst = static_cast<size_t>(y) * static_cast<size_t>(out_width) * 3u;
            std::memcpy(cropped.data() + dst, frame.data() + src, static_cast<size_t>(out_width) * 3u);
        }
        frame.swap(cropped);
    }
    return frame;
}

