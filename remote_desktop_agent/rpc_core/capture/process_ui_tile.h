#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

/// 单窗 RGB24 瓦片（屏幕原点坐标），供采集后端填充并由 ProcessUiCapture 合成。
struct ProcessUiWindowTile {
    HWND hwnd = nullptr;
    RECT rect_screen{};
    int z_order = 0;
    std::vector<uint8_t> rgb;
    int w = 0;
    int h = 0;
    int origin_left = 0;
    int origin_top = 0;
};
