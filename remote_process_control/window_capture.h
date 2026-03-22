#pragma once
#include <windows.h>
#include <vector>
#include <cstddef>

class WindowCapture 
{
public:
    // 采集窗口内容，返回BGR格式的字节流
    std::vector<uint8_t> capture(HWND hwnd, int width, int height);
};

