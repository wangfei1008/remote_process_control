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

std::vector<uint8_t> WindowFrameGrabber::capture_all_windows_image(GdiCapture& gdi_capture,
                                                                   DWORD pid,
                                                                   HWND anchor_hwnd,
                                                                   int max_below_main_px,
                                                                   int& out_width,
                                                                   int& out_height,
                                                                   int& out_min_left,
                                                                   int& out_min_top)
{
    struct Window_info {
        HWND hwnd;
        RECT rect;
        std::vector<uint8_t> image;
    };

    std::vector<HWND> hwnds;
    static std::vector<HWND>* s_zorder_vec = nullptr;
    s_zorder_vec = &hwnds;
    EnumWindows([](HWND hwnd, LPARAM l_param) -> BOOL {
        DWORD win_pid = 0;
        GetWindowThreadProcessId(hwnd, &win_pid);
        if (IsWindowVisible(hwnd) && win_pid == static_cast<DWORD>(l_param) && s_zorder_vec) {
            s_zorder_vec->push_back(hwnd);
        }
        return TRUE;
    }, static_cast<LPARAM>(pid));
    s_zorder_vec = nullptr;

    if (hwnds.empty()) {
        out_width = 0;
        out_height = 0;
        out_min_left = 0;
        out_min_top = 0;
        return {};
    }

    std::vector<Window_info> windows;
    for (HWND hwnd : hwnds) {
        RECT win_rect{};
        if (!window_rect_utils::get_effective_window_rect(hwnd, win_rect)) continue;
        const int width = win_rect.right - win_rect.left;
        const int height = win_rect.bottom - win_rect.top;
        if (width <= 0 || height <= 0) continue;

        std::vector<uint8_t> img = gdi_capture.capture(hwnd, width, height, true);
        if (img.empty()) continue;
        windows.push_back({hwnd, win_rect, std::move(img)});
    }

    if (windows.empty()) {
        out_width = 0;
        out_height = 0;
        out_min_left = 0;
        out_min_top = 0;
        return {};
    }

    int min_left = INT_MAX;
    int min_top = INT_MAX;
    int max_right = INT_MIN;
    int max_bottom = INT_MIN;
    for (const auto& win : windows) {
        min_left = (std::min)(min_left, static_cast<int>(win.rect.left));
        min_top = (std::min)(min_top, static_cast<int>(win.rect.top));
        max_right = (std::max)(max_right, static_cast<int>(win.rect.right));
        max_bottom = (std::max)(max_bottom, static_cast<int>(win.rect.bottom));
    }

    RECT clip_rect{min_left, min_top, max_right, max_bottom};
    if (anchor_hwnd && IsWindow(anchor_hwnd) && max_below_main_px > 0) {
        RECT anchor_rect{};
        if (window_rect_utils::get_effective_window_rect(anchor_hwnd, anchor_rect)) {
            const LONG cap_bottom = anchor_rect.bottom + static_cast<LONG>(max_below_main_px);
            if (clip_rect.bottom > cap_bottom) clip_rect.bottom = cap_bottom;
            if (clip_rect.bottom <= clip_rect.top) clip_rect.bottom = clip_rect.top + 2;
        }
    }

    int total_width = clip_rect.right - clip_rect.left;
    int total_height = clip_rect.bottom - clip_rect.top;
    total_width = (total_width + 1) & ~1;
    total_height = (total_height + 1) & ~1;
    out_width = total_width;
    out_height = total_height;
    out_min_left = clip_rect.left;
    out_min_top = clip_rect.top;

    std::vector<uint8_t> merged_image(static_cast<size_t>(total_width) * static_cast<size_t>(total_height) * 3u, 0);
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        const auto& win = *it;
        RECT inter{};
        if (!IntersectRect(&inter, &win.rect, &clip_rect)) continue;

        const int src_w = win.rect.right - win.rect.left;
        const int src_x = inter.left - win.rect.left;
        const int src_y = inter.top - win.rect.top;
        const int dst_x = inter.left - clip_rect.left;
        const int dst_y = inter.top - clip_rect.top;
        const int inter_w = inter.right - inter.left;
        const int inter_h = inter.bottom - inter.top;
        if (inter_w <= 0 || inter_h <= 0 || src_w <= 0) continue;

        for (int y = 0; y < inter_h; ++y) {
            const size_t src_row = (static_cast<size_t>(src_y + y) * static_cast<size_t>(src_w) + static_cast<size_t>(src_x)) * 3u;
            const size_t dst_row = (static_cast<size_t>(dst_y + y) * static_cast<size_t>(total_width) + static_cast<size_t>(dst_x)) * 3u;
            const size_t copy_bytes = static_cast<size_t>(inter_w) * 3u;
            if (src_row + copy_bytes > win.image.size() || dst_row + copy_bytes > merged_image.size()) continue;
            std::memcpy(merged_image.data() + dst_row, win.image.data() + src_row, copy_bytes);
        }
    }

    return merged_image;
}

