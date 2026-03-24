#pragma once
#include <windows.h>
#include <vector>
#include <cstddef>

class WindowCapture 
{
public:
    // Capture window content and return a BGR byte buffer.
    std::vector<uint8_t> capture(HWND hwnd, int width, int height);
};

