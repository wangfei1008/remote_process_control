// ============================================================
// win32_window.cpp
// ============================================================

#include "win32_window.h"

#if defined(_WIN32)
#  include <cwchar>

// character_conversion 仅在 Windows 下使用
#include "common/character_conversion.h"

namespace win32 {

namespace {

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#  define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

using DwmGetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);

DwmGetWindowAttributeFn get_dwm_fn() {
    static DwmGetWindowAttributeFn s_fn = [] {
        HMODULE m = LoadLibraryA("dwmapi.dll");
        if (!m) return static_cast<DwmGetWindowAttributeFn>(nullptr);
        return reinterpret_cast<DwmGetWindowAttributeFn>(
            GetProcAddress(m, "DwmGetWindowAttribute"));
    }();
    return s_fn;
}

// 过滤不应该出现在采集列表里的系统辅助窗口类
bool is_system_helper_class(const std::string& cls) {
    return cls == "IME" || cls == "MSCTFIME UI" || cls == "SoPY_Status";
}

} // namespace

// ---- 基础判断 -----------------------------------------------

bool Window::is_valid(HWND h) const {
    return h != nullptr && IsWindow(h) != FALSE;
}

bool Window::is_visible(HWND h) const {
    return is_valid(h) && IsWindowVisible(h) != FALSE;
}

bool Window::is_ownerless_top_level(HWND h) const {
    if (!is_valid(h)) return false;
    return GetWindow(h, GW_OWNER) == nullptr;
}

// ---- 属性读取 -----------------------------------------------

DWORD Window::pid(HWND h) const {
    if (!is_valid(h)) return 0;
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    return p;
}

LONG_PTR Window::style(HWND h) const {
    if (!is_valid(h)) return 0;
    return GetWindowLongPtr(h, GWL_STYLE);
}

LONG_PTR Window::ex_style(HWND h) const {
    if (!is_valid(h)) return 0;
    return GetWindowLongPtr(h, GWL_EXSTYLE);
}

bool Window::get_rect(HWND h, RECT& out) const {
    if (!is_valid(h)) return false;
    RECT rc{};
    if (!GetWindowRect(h, &rc)) return false;
    out = rc;
    return true;
}

bool Window::get_effective_rect(HWND h, RECT& out) const {
    if (!get_rect(h, out)) return false;
    if (auto fn = get_dwm_fn()) {
        RECT ext{};
        if (SUCCEEDED(fn(h, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext)))
            && ext.right > ext.left && ext.bottom > ext.top) {
            out = ext;
        }
    }
    return true;
}

bool Window::query_dwm_dword(HWND h, DWORD attr, DWORD& out) const {
    out = 0;
    if (!is_valid(h)) return false;
    auto fn = get_dwm_fn();
    if (!fn) return false;
    return SUCCEEDED(fn(h, attr, &out, sizeof(out)));
}

std::string Window::text_utf8(HWND h) const {
    if (!is_valid(h)) return {};
    WCHAR buf[512]{};
    const int n = GetWindowTextW(h, buf, static_cast<int>(std::size(buf)));
    if (n <= 0) return {};
    return wide_to_utf8(std::wstring(buf, buf + n));
}

std::string Window::class_name_utf8(HWND h) const {
    if (!is_valid(h)) return {};
    WCHAR buf[256]{};
    const int n = GetClassNameW(h, buf, static_cast<int>(std::size(buf)));
    if (n <= 0) return {};
    return wide_to_utf8(std::wstring(buf, buf + n));
}

// ---- 快照 ---------------------------------------------------

WindowInfo Window::snapshot(HWND h) const {
    WindowInfo info;
    info.hwnd  = h;
    info.valid = is_valid(h);
    if (!info.valid) return info;

    info.pid       = pid(h);
    info.visible   = is_visible(h);
    info.ownerless = is_ownerless_top_level(h);
    info.style     = style(h);
    info.ex_style  = ex_style(h);

    RECT rc{};
    if (get_effective_rect(h, rc)) {
        info.rect_ok     = true;
        info.rect_screen = rc;
    }

    info.title      = text_utf8(h);
    info.class_name = class_name_utf8(h);
    return info;
}

// ---- 枚举 ---------------------------------------------------

void Window::enumerate_top_level(const std::function<bool(HWND)>& visitor) const {
    if (!visitor) return;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* fn = reinterpret_cast<const std::function<bool(HWND)>*>(lp);
            return (fn && *fn && (*fn)(h)) ? TRUE : FALSE;
        },
        reinterpret_cast<LPARAM>(&visitor));
}

std::vector<WindowInfo> Window::enumerate_visible_top_level(DWORD target_pid) const {
    if (target_pid == 0) return {};
    std::vector<WindowInfo> out;
    int next_z = 0;
    enumerate_top_level([&](HWND h) -> bool {
        if (pid(h) != target_pid)  return true;
        if (!is_visible(h))        return true;
        if (is_system_helper_class(class_name_utf8(h))) return true;

        WindowInfo info  = snapshot(h);
        info.z_order     = next_z++;
        out.push_back(std::move(info));
        return true;
    });
    return out;
}

} // namespace win32

#else // !_WIN32 -----------------------------------------------

namespace win32 {

bool Window::is_valid(HWND) const                          { return false; }
bool Window::is_visible(HWND) const                        { return false; }
bool Window::is_ownerless_top_level(HWND) const            { return false; }
DWORD    Window::pid(HWND) const                           { return 0; }
LONG_PTR Window::style(HWND) const                         { return 0; }
LONG_PTR Window::ex_style(HWND) const                      { return 0; }
bool Window::get_rect(HWND, RECT&) const                   { return false; }
bool Window::get_effective_rect(HWND, RECT&) const         { return false; }
bool Window::query_dwm_dword(HWND, DWORD, DWORD& v) const { v = 0; return false; }
std::string Window::text_utf8(HWND) const                  { return {}; }
std::string Window::class_name_utf8(HWND) const            { return {}; }
WindowInfo  Window::snapshot(HWND) const                   { return {}; }
void Window::enumerate_top_level(const std::function<bool(HWND)>&) const {}
std::vector<WindowInfo> Window::enumerate_visible_top_level(DWORD) const { return {}; }

} // namespace win32

#endif