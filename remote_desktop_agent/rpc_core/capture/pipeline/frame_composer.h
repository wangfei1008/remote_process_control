#pragma once

// ============================================================
// frame_composer.h
// 帧合成器：把多个窗口瓦片合并成一张图
//
// 原代码问题：compose_bbox / compose_linear 是匿名 namespace 里
// 的自由函数，和采集逻辑混在同一个 .cpp 文件中，无法独立测试。
//
// 职责边界：
//   YES - 多瓦片合成（Bbox / Horizontal / Vertical / Grid）
//   NO  - 不做任何 Win32 调用，纯内存像素操作
//   NO  - 不决定何时采集、哪个窗口（那是上游的事）
// ============================================================

#include "../backend/window_tile.h"
#include <cstdint>
#include <span>
#include <vector>

namespace capture {

// ----------------------------------------------------------
// CompositeLayout：合成布局模式
// ----------------------------------------------------------
enum class CompositeLayout {
    Bbox,        // 按屏幕坐标取联合 bbox，各窗口按 z 序叠加
    Horizontal,  // 水平平铺，间距 padding_px
    Vertical,    // 垂直平铺，间距 padding_px
    Grid,        // 网格，列数 grid_columns
};

// ----------------------------------------------------------
// ComposedFrame：合成输出（纯数据）
// ----------------------------------------------------------
struct ComposedFrame {
    bool ok           = false;
    int  width        = 0;
    int  height       = 0;
    int  origin_left  = 0;   // 屏幕坐标原点（Bbox 模式有意义）
    int  origin_top   = 0;
    std::vector<uint8_t> pixels;  // RGB24，行优先，无 padding
};

// ----------------------------------------------------------
// FrameComposer
// ----------------------------------------------------------
class FrameComposer {
public:
    struct Options {
        CompositeLayout layout       = CompositeLayout::Bbox;
        int             padding_px   = 8;
        int             grid_columns = 2;
    };

    // 从运行时配置读取选项（RPC_COMPOSITE_LAYOUT 等）
    static Options options_from_config();

    // 主接口：给定瓦片列表，返回合成结果
    // 单瓦片时直接复制，避免不必要的重新分配
    ComposedFrame compose(std::span<const WindowTile> tiles,
                          const Options&              opts) const;

private:
    // Bbox：按屏幕坐标联合，z 序叠加（前景遮挡背景）
    ComposedFrame compose_bbox(std::span<const WindowTile> tiles) const;

    // Linear/Grid：线性或网格平铺
    ComposedFrame compose_linear(std::span<const WindowTile> tiles,
                                  const Options&              opts) const;

    static void make_even(int& v) { if (v > 0) v &= ~1; }
};

} // namespace capture
