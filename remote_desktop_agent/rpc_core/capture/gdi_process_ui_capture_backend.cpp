#include "capture/gdi_process_ui_capture_backend.h"

#include "common/window_rect_utils.h"

#include <algorithm>
#include <cstring>
#include <vector>

#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x00000001
#endif

namespace {

std::vector<uint8_t> gdi_capture_window_rgb24(HWND hwnd, int width, int height, bool includeNonClient)
{
    std::vector<uint8_t> buffer;
    if (!hwnd || width <= 0 || height <= 0) return buffer;

    HDC hWndDC = includeNonClient ? GetWindowDC(hwnd) : GetDC(hwnd);
    if (!hWndDC) return buffer;
    HDC hMemDC = CreateCompatibleDC(hWndDC);
    if (!hMemDC) {
        ReleaseDC(hwnd, hWndDC);
        return buffer;
    }

    HBITMAP hBitmap = CreateCompatibleBitmap(hWndDC, width, height);
    if (!hBitmap) {
        DeleteDC(hMemDC);
        ReleaseDC(hwnd, hWndDC);
        return buffer;
    }
    HGDIOBJ oldBitmap = SelectObject(hMemDC, hBitmap);

    BOOL ok = FALSE;
    HDC hScreenDC = nullptr;
    bool useScreenRectCopy = false;
    int srcX = 0;
    int srcY = 0;

    if (includeNonClient) {
        RECT wr{};
        if (window_rect_utils::get_effective_window_rect(hwnd, wr)) {
            hScreenDC = GetDC(nullptr);
            if (hScreenDC) {
                useScreenRectCopy = true;
                srcX = wr.left;
                srcY = wr.top;
            }
        }
    }

    if (includeNonClient) {
        if (useScreenRectCopy) {
            ok = BitBlt(hMemDC, 0, 0, width, height, hScreenDC, srcX, srcY, SRCCOPY | CAPTUREBLT);
        }
        if (!ok) {
            ok = BitBlt(hMemDC, 0, 0, width, height, hWndDC, 0, 0, SRCCOPY | CAPTUREBLT);
        }
        if (!ok) {
            ok = PrintWindow(hwnd, hMemDC, 0);
        }
    } else {
        ok = PrintWindow(hwnd, hMemDC, PW_CLIENTONLY);
        if (!ok) {
            ok = BitBlt(hMemDC, 0, 0, width, height, hWndDC, 0, 0, SRCCOPY | CAPTUREBLT);
        }
    }

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    std::vector<uint8_t> raw(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    int lines = GetDIBits(
        hMemDC,
        hBitmap,
        0,
        height,
        raw.data(),
        reinterpret_cast<BITMAPINFO*>(&bi),
        DIB_RGB_COLORS);

    if (lines != height) {
        raw.clear();
    }

    buffer.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);

    for (int i = 0, j = 0; i < width * height * 4; i += 4, j += 3) {
        buffer[static_cast<size_t>(j) + 0] = raw[static_cast<size_t>(i) + 2];
        buffer[static_cast<size_t>(j) + 1] = raw[static_cast<size_t>(i) + 1];
        buffer[static_cast<size_t>(j) + 2] = raw[static_cast<size_t>(i) + 0];
    }

    SelectObject(hMemDC, oldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(hwnd, hWndDC);
    if (hScreenDC) {
        ReleaseDC(nullptr, hScreenDC);
    }

    if (!ok || raw.empty()) {
        buffer.clear();
    }
    return buffer;
}

std::vector<uint8_t> capture_main_window_image_gdi(HWND hwnd,
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

    auto frame = gdi_capture_window_rgb24(hwnd, width, height, include_non_client);
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

} // namespace

bool GdiProcessUiCaptureBackend::probe()
{
    return true;
}

GdiProcessUiCaptureBackend::GdiProcessUiCaptureBackend() = default;

GdiProcessUiCaptureBackend::~GdiProcessUiCaptureBackend() = default;

bool GdiProcessUiCaptureBackend::capture_tiles(const std::vector<ProcessSurfaceInfo>& surfaces,
                                               std::vector<ProcessUiWindowTile>& tiles,
                                               uint64_t /*now_unix_ms*/)
{
    tiles.clear();
    if (surfaces.empty()) return false;
    tiles.reserve(surfaces.size());
    for (const auto& s : surfaces) {
        ProcessUiWindowTile out{};
        out.hwnd = s.hwnd;
        out.rect_screen = s.rect_screen;
        out.z_order = s.z_order;
        int cap_l = 0;
        int cap_t = 0;
        out.rgb = capture_main_window_image_gdi(s.hwnd, out.w, out.h, cap_l, cap_t, true);
        if (out.rgb.empty() || out.w <= 0 || out.h <= 0) return false;
        const size_t expected = static_cast<size_t>(out.w) * static_cast<size_t>(out.h) * 3u;
        if (out.rgb.size() != expected) return false;
        out.origin_left = cap_l;
        out.origin_top = cap_t;
        tiles.push_back(std::move(out));
    }
    return true;
}

void GdiProcessUiCaptureBackend::reset_session_recovery()
{
}
