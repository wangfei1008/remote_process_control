#pragma once
#include <windows.h>
#include <vector>
#include <cstddef>

class GdiCapture
{
public:
    // 采集指定窗口区域并返回 RGB24 字节缓冲。
    // 当 includeNonClient=true 时采集整个窗口（含标题栏/边框）。
    std::vector<uint8_t> capture(HWND hwnd, int width, int height, bool includeNonClient);
};

