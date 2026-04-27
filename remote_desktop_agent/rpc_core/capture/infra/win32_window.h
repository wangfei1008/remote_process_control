#pragma once

// ============================================================
// win32_window.h
// Win32 窗口操作：纯工具，无状态
//
// 职责边界：
//   YES - IsWindow / IsWindowVisible / GetWindowRect / DWM 边框
//   YES - GetWindowText / GetClassName / GetWindowLongPtr
//   YES - EnumWindows 枚举入口
//   NO  - 不包含"哪个窗口最好"的策略判断
//   NO  - 不包含进程相关逻辑
// ============================================================

#include "win32_types.h"
#include <functional>
#include <string>
#include <vector>

namespace win32 {

// ----------------------------------------------------------
// WindowInfo：窗口基础快照（纯数据，无行为）
// ----------------------------------------------------------
struct WindowInfo {
    HWND      hwnd       = nullptr;
    DWORD     pid        = 0;
    int       z_order    = 0;      // enumerate_visible_top_level 填写

    bool      valid      = false;
    bool      visible    = false;
    bool      ownerless  = false;  // GetWindow(GW_OWNER) == NULL

    LONG_PTR  style      = 0;
    LONG_PTR  ex_style   = 0;

    bool      rect_ok    = false;
    RECT      rect_screen{};

    std::string title;
    std::string class_name;
};

// ----------------------------------------------------------
// Window：窗口操作工具
// ----------------------------------------------------------
class Window {
public:
    // ---- 基础判断 ----------------------------------------
    bool is_valid(HWND hwnd)              const;
    bool is_visible(HWND hwnd)            const;
    bool is_ownerless_top_level(HWND hwnd) const;

    // ---- 属性读取 ----------------------------------------
    DWORD    pid(HWND hwnd)      const;
    LONG_PTR style(HWND hwnd)    const;
    LONG_PTR ex_style(HWND hwnd) const;

    // GetWindowRect
    bool get_rect(HWND hwnd, RECT& out) const;
    // 优先 DWM extended frame bounds，回退 GetWindowRect
    bool get_effective_rect(HWND hwnd, RECT& out) const;

    // DWM DWORD attribute（如 DWMWA_CLOAKED）
    bool query_dwm_dword(HWND hwnd, DWORD attr, DWORD& out) const;

    // UTF-8 标题 / 类名
    std::string text_utf8(HWND hwnd)       const;
    std::string class_name_utf8(HWND hwnd) const;

    // ---- 快照 -------------------------------------------
    WindowInfo snapshot(HWND hwnd) const;

    // ---- 枚举 -------------------------------------------
    // 枚举所有顶层窗口；visitor 返回 false 提前终止
    void enumerate_top_level(
        const std::function<bool(HWND)>& visitor) const;

    // 枚举指定 PID 的可见顶层窗口（过滤 IME/工具条）
    std::vector<WindowInfo> enumerate_visible_top_level(DWORD pid) const;
};

} // namespace win32