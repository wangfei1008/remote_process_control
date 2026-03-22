// desktop_capture.hpp
#pragma once
#include <windows.h>
#include <cstdint>

class DesktopCapture {
public:
    DesktopCapture();
    ~DesktopCapture();
    bool grab_frame(uint8_t* buffer, int width, int height);
    int getWidth() const { return width; }
    int getHeight() const { return height; }
private:
    int width;
    int height;
    HDC hScreenDC;
    HDC hMemoryDC;
    HBITMAP hBitmap;
    BITMAPINFO bmi;
};

