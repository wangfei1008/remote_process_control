#include "capture/gdi_capture.h"
#include "common/window_rect_utils.h"

// 某些旧版 SDK 头文件中没有 PW_CLIENTONLY。
#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x00000001
#endif

std::vector<uint8_t> GdiCapture::capture(HWND hwnd, int width, int height, bool includeNonClient)
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
        // 全窗口场景优先使用屏幕矩形拷贝，这样更稳定地包含标题栏/边框，
        // 避免部分窗口在 WindowDC/PrintWindow 下只返回客户区。
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
        // 全窗口模式必须包含非客户区。
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
        // 客户区模式优先采客户区，避免 DXGI 回退路径的非客户区抖动。
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

    std::vector<uint8_t> raw(width * height * 4);

    int lines = GetDIBits(
        hMemDC,
        hBitmap,
        0,
        height,
        raw.data(),
        reinterpret_cast<BITMAPINFO*>(&bi),
        DIB_RGB_COLORS
    );

    if (lines != height) {
        raw.clear();
    }

    buffer.resize(width * height * 3);

    for (int i = 0, j = 0; i < width * height * 4; i += 4, j += 3) {
        buffer[j + 0] = raw[i + 2];
        buffer[j + 1] = raw[i + 1];
        buffer[j + 2] = raw[i + 0];
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

