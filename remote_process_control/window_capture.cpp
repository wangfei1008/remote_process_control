#include "window_capture.h"

// PW_CLIENTONLY isn't available on some older SDK headers.
#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x00000001
#endif

std::vector<uint8_t> WindowCapture::capture(HWND hwnd, int width, int height)
{
    std::vector<uint8_t> buffer;
    if (!hwnd) return buffer;

   // SetForegroundWindow(hwnd);
    // Use client DC to keep fallback BitBlt aligned with client-sized capture.
    HDC hWindowDC = GetDC(hwnd);
    HDC hMemDC = CreateCompatibleDC(hWindowDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hWindowDC, width, height);
    HGDIOBJ oldBitmap = SelectObject(hMemDC, hBitmap);

    // Copy window content into a memory DC.
    //BitBlt(hMemDC, 0, 0, width, height, hWindowDC, 0, 0, SRCCOPY | CAPTUREBLT);
    // Prefer PrintWindow for better compatibility across window types.
    // IMPORTANT: we capture with (width,height) that comes from client-rect.
    // Using PW_RENDERFULLCONTENT with client dimensions can misalign content and
    // produce black bars, so we only try client-only first and otherwise fall back
    // to BitBlt (still aligned to the provided client-sized bitmap).
    BOOL ok = PrintWindow(hwnd, hMemDC, PW_CLIENTONLY);
    if (!ok) {
        BitBlt(hMemDC, 0, 0, width, height, hWindowDC, 0, 0, SRCCOPY | CAPTUREBLT);
    }

    // Read bitmap data.
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height; // top-down
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;

    int rowSize = ((width * 3 + 3) & ~3); // each row is 4-byte aligned
	//int rowSize = width * 3; 
    //buffer.resize(rowSize * height);

    std::vector<uint8_t> raw(rowSize * height);
    int lines = GetDIBits(hMemDC, hBitmap, 0, height, raw.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    // Strip per-row padding.
    buffer.resize(width * 3 * height);
    for (int y = 0; y < height; ++y) {
        std::memcpy(
            buffer.data() + y * width * 3,
            raw.data() + y * rowSize,
            width * 3
        );
    }
    for (size_t i = 0; i < buffer.size(); i += 3) {
        std::swap(buffer[i], buffer[i + 2]);
    }

    // Cleanup.
    SelectObject(hMemDC, oldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemDC);
    ReleaseDC(hwnd, hWindowDC);

    return buffer;
}

