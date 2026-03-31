#pragma once
#include <windows.h>
#include <vector>
#include <cstddef>

class GdiCapture
{
public:
    // Capture selected window region and return an RGB24 byte buffer.
    // includeNonClient=true captures full window (title bar/borders included).
    std::vector<uint8_t> capture(HWND hwnd, int width, int height, bool includeNonClient);
};

