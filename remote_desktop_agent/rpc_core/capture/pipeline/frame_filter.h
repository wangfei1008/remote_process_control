#pragma once

// ============================================================
// frame_filter.h
// 可疑帧过滤器（原 capture_rgb_heuristics 的语义提升）
//
// 设计意图：
//   原代码是一组自由函数，调用方不清楚"为什么要调"以及"调完怎么办"。
//   本类把三个检测函数封装为一个有意图的概念：
//   "这一帧是否应该被丢弃，以及如何修复"。
//
// 职责边界：
//   YES - 基于像素统计判断帧质量（全黑/低方差/顶部黑条）
//   YES - 用上一帧修复顶部黑条
//   NO  - 不做任何 Win32 调用，输入全是内存中的像素数据
//   NO  - 不决定"丢弃后做什么"（那是调用方的事）
// ============================================================

#include <cstdint>
#include <span>
#include <vector>

namespace capture {

class FrameFilter {
public:
    // 主判断：这帧是否应该被丢弃
    // 顺序：全黑 → 低方差 → 顶部黑条，任一为 true 则丢弃
    bool should_discard(std::span<const uint8_t> rgb, int w, int h) const;

    // 修复：用上一帧的顶部区域覆盖当前帧（处理顶部黑条）
    // prev_rgb 尺寸必须与 rgb 相同，否则无操作
    void repair_top_strip(std::vector<uint8_t>&        rgb,
                          int                          w,
                          int                          h,
                          std::span<const uint8_t>     prev_rgb) const;

private:
    // ---- 子检测 ---------------------------------------------
    // 采样检测（不遍历全部像素，快速判断）

    // 90% 以上像素 RGB 之和 ≤ 24 → 全黑
    bool is_mostly_black(std::span<const uint8_t> rgb, int w, int h) const;

    // 亮度方差 < 12.0 → 内容几乎无变化（黑屏/纯色）
    bool is_low_variance(std::span<const uint8_t> rgb, int w, int h) const;

    // 顶部 1/6 区域 ≥ 80% 黑，且下半部分 < 75% 黑 → 顶部黑条
    bool has_top_black_strip(std::span<const uint8_t> rgb, int w, int h) const;
};

} // namespace capture
