// desktop_capture.hpp
#pragma once
#include <windows.h>
#include <cstdint>

class DesktopCapture {
public:
    DesktopCapture();
    ~DesktopCapture();
    bool grab_frame(uint8_t* buffer, int width, int height);
    int get_width() const { return width; }
    int get_height() const { return height; }
private:
    int width;
    int height;
    HDC hScreenDC;
    HDC hMemoryDC;
    HBITMAP hBitmap;
    BITMAPINFO bmi;
};

