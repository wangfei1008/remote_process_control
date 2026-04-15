#pragma once

#if defined(_WIN32)
#include <windows.h>
#else
// 非 Windows 平台下用于 clangd 索引的最小占位定义（不参与实际运行）。
using HWND = void*;
using DWORD = unsigned long;
using LONG_PTR = long long;
struct RECT {
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
};
#endif

#include <string>
#include <vector>

////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： Win32 窗口操作封装（基础查询 + 快照输出）
//
// 作者：WangFei
// 时间： 2026-04-15
// 修改:
//              1、2026-04-15创建
//
//详细功能说明：
//- 负责 IsWindow/IsWindowVisible/GetWindowRect/GetClassName/GetWindowText
//- 负责 GetWindowThreadProcessId/GetWindow(GW_OWNER)/GetWindowLongPtr(style/ex_style)
//- 负责 DWM 扩展边框 rect（优先 extended frame bounds）
//- 不包含窗口“主窗/最优窗”策略与评分（这些属于策略层）
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

class window_ops {
public:
    struct window_info {
        HWND hwnd = nullptr;
        DWORD pid = 0;
        int z_order = 0;

        bool valid = false;
        bool visible = false;
        bool ownerless = false;

        LONG_PTR style = 0;
        LONG_PTR ex_style = 0;

        bool rect_ok = false;
        RECT rect_screen{};

        std::string title;
        std::string class_name;
    };

    bool is_valid(HWND hwnd) const;
    bool is_visible(HWND hwnd) const;
    bool is_ownerless_top_level(HWND hwnd) const;

    DWORD get_window_pid(HWND hwnd) const;

    LONG_PTR get_style(HWND hwnd) const;
    LONG_PTR get_ex_style(HWND hwnd) const;

    bool get_window_rect(HWND hwnd, RECT& out_rect) const;
    // Prefer DWM extended frame bounds when available; fallback to GetWindowRect.
    bool get_effective_window_rect(HWND hwnd, RECT& out_rect) const;

    std::string get_window_text_utf8(HWND hwnd) const;
    std::string get_window_class_utf8(HWND hwnd) const;

    // Query a DWM attribute that returns DWORD (e.g. DWMWA_CLOAKED).
    bool query_dwm_attribute_dword(HWND hwnd, DWORD attribute, DWORD& out_value) const;

    // Visible top-level windows for PID (same rules as legacy capture_all_windows).
    std::vector<window_info> enumerate_visible_top_level(DWORD pid) const;

    window_info snapshot(HWND hwnd) const;
};

// 兼容外部命名习惯。
using WindowOps = window_ops;

