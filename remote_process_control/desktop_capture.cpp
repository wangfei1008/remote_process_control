// desktop_capture.cpp
#include "desktop_capture.hpp"

DesktopCapture::DesktopCapture() {
    hScreenDC = GetDC(nullptr);
    hMemoryDC = CreateCompatibleDC(hScreenDC);

    width = GetSystemMetrics(SM_CXSCREEN);
    height = GetSystemMetrics(SM_CYSCREEN);

    hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    SelectObject(hMemoryDC, hBitmap);

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down bitmap
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
}

DesktopCapture::~DesktopCapture() {
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(nullptr, hScreenDC);
}

bool DesktopCapture::grab_frame(uint8_t* buffer, int width, int height) {
    if (!buffer) return false;
    if (width != this->width || height != this->height) return false;

    BOOL ret = BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, 0, 0, SRCCOPY | CAPTUREBLT);
    if (!ret) return false;

    int ret2 = GetDIBits(hMemoryDC, hBitmap, 0, height, buffer, &bmi, DIB_RGB_COLORS);
    return ret2 != 0;
}

