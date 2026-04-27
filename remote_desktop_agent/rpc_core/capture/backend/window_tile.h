#pragma once

// ============================================================
// window_tile.h
// 单窗口 RGB24 瓦片（采集后端输出，合成器输入）
// ============================================================

#include "../infra/win32_types.h"
#include <cstdint>
#include <vector>

namespace capture {

struct WindowTile {
    HWND             hwnd        = nullptr;
    RECT             rect_screen{};
    int              z_order     = 0;

    std::vector<uint8_t> rgb;   // RGB24，行优先，无 padding
    int              w           = 0;
    int              h           = 0;
    int              origin_left = 0;  // 屏幕坐标原点
    int              origin_top  = 0;
};

} // namespace capture
