#pragma once

#include <string>
#include <vector>
#include <windows.h>

struct ProcessSurfaceInfo {
    HWND hwnd = nullptr;
    std::string title;
    std::string class_name;
    RECT rect_screen{};
    int z_order = 0;
};

class ProcessSurfaceEnumerator {
public:
    // Visible top-level HWNDs for PID (same rules as legacy capture_all_windows).
    static std::vector<ProcessSurfaceInfo> enumerate_visible_top_level(DWORD pid);
};
