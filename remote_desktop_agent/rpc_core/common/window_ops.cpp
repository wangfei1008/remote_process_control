#include "window_ops.h"

#if defined(_WIN32)
#include <cwchar>

#include <cstring>

namespace {

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

using DwmGetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);

static DwmGetWindowAttributeFn get_dwm_get_window_attribute_fn()
{
    static DwmGetWindowAttributeFn s_fn = []() -> DwmGetWindowAttributeFn {
        HMODULE module = LoadLibraryA("dwmapi.dll");
        if (!module) return nullptr;
        auto fn = reinterpret_cast<DwmGetWindowAttributeFn>(GetProcAddress(module, "DwmGetWindowAttribute"));
        return fn;
    }();
    return s_fn;
}

static std::string utf16_to_utf8(const WCHAR* wstr, int len)
{
    if (!wstr || len <= 0) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, wstr, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, out.data(), need, nullptr, nullptr);
    return out;
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          判断 HWND 是否有效（IsWindow）
/// @参数
///          hwnd--窗口句柄
/// @返回值
///          true--有效
///          false--无效
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
bool window_ops::is_valid(HWND hwnd) const
{
    return hwnd != nullptr && IsWindow(hwnd) != FALSE;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          判断窗口是否可见（IsWindowVisible）
/// @参数
///          hwnd--窗口句柄
/// @返回值
///          true--可见
///          false--不可见
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
bool window_ops::is_visible(HWND hwnd) const
{
    return is_valid(hwnd) && IsWindowVisible(hwnd) != FALSE;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          判断窗口是否无 owner（GetWindow(GW_OWNER) == NULL）
/// @参数
///          hwnd--窗口句柄
/// @返回值
///          true--无 owner
///          false--有 owner 或无效
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
bool window_ops::is_ownerless_top_level(HWND hwnd) const
{
    if (!is_valid(hwnd)) return false;
    return GetWindow(hwnd, GW_OWNER) == NULL;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          获取窗口所属进程 PID（GetWindowThreadProcessId）
/// @参数
///          hwnd--窗口句柄
/// @返回值
///          pid--所属进程 PID；失败返回 0
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
DWORD window_ops::get_window_pid(HWND hwnd) const
{
    if (!is_valid(hwnd)) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          获取窗口 style（GWL_STYLE）
/// @参数
///          hwnd--窗口句柄
/// @返回值
///          style--style；失败返回 0
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
LONG_PTR window_ops::get_style(HWND hwnd) const
{
    if (!is_valid(hwnd)) return 0;
    return GetWindowLongPtr(hwnd, GWL_STYLE);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          获取窗口 ex_style（GWL_EXSTYLE）
/// @参数
///          hwnd--窗口句柄
/// @返回值
///          ex_style--ex_style；失败返回 0
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
LONG_PTR window_ops::get_ex_style(HWND hwnd) const
{
    if (!is_valid(hwnd)) return 0;
    return GetWindowLongPtr(hwnd, GWL_EXSTYLE);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          获取窗口屏幕坐标 rect（GetWindowRect）
/// @参数
///          hwnd--窗口句柄
///          out_rect--输出 rect
/// @返回值
///          true--成功
///          false--失败
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
bool window_ops::get_window_rect(HWND hwnd, RECT& out_rect) const
{
    if (!is_valid(hwnd)) return false;
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return false;
    out_rect = rc;
    return true;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          获取有效窗口 rect：优先 DWM 扩展边框，否则退回 GetWindowRect
/// @参数
///          hwnd--窗口句柄
///          out_rect--输出 rect
/// @返回值
///          true--成功
///          false--失败
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
bool window_ops::get_effective_window_rect(HWND hwnd, RECT& out_rect) const
{
    if (!get_window_rect(hwnd, out_rect)) return false;

    if (auto fn = get_dwm_get_window_attribute_fn()) {
        RECT ext{};
        if (SUCCEEDED(fn(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof(ext))) && ext.right > ext.left &&
            ext.bottom > ext.top) {
            out_rect = ext;
        }
    }
    return true;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          查询 DWM window attribute（DWORD）：如 DWMWA_CLOAKED
/// @参数
///          hwnd--窗口句柄
///          attribute--DWM attribute id
///          out_value--输出 DWORD 值
/// @返回值
///          true--查询成功
///          false--失败或 DWM 不可用
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
bool window_ops::query_dwm_attribute_dword(HWND hwnd, DWORD attribute, DWORD& out_value) const
{
    out_value = 0;
    if (!is_valid(hwnd)) return false;
    if (auto fn = get_dwm_get_window_attribute_fn()) {
        return SUCCEEDED(fn(hwnd, attribute, &out_value, sizeof(out_value))) != FALSE;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          获取窗口标题（UTF-8，GetWindowTextW）
/// @参数
///          hwnd--窗口句柄
/// @返回值
///          title_utf8--标题；失败返回空串
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
std::string window_ops::get_window_text_utf8(HWND hwnd) const
{
    if (!is_valid(hwnd)) return {};
    WCHAR buf[512]{};
    const int n = GetWindowTextW(hwnd, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    if (n <= 0) return {};
    return utf16_to_utf8(buf, n);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          获取窗口类名（UTF-8，GetClassNameW）
/// @参数
///          hwnd--窗口句柄
/// @返回值
///          class_utf8--类名；失败返回空串
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
std::string window_ops::get_window_class_utf8(HWND hwnd) const
{
    if (!is_valid(hwnd)) return {};
    WCHAR buf[256]{};
    const int n = GetClassNameW(hwnd, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    if (n <= 0) return {};
    return utf16_to_utf8(buf, n);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          输出窗口基础快照：pid/title/class/rect/visible/ownerless/style/ex_style
/// @参数
///          hwnd--窗口句柄
/// @返回值
///          window_info--快照；无效窗口 valid=false
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
window_ops::window_info window_ops::snapshot(HWND hwnd) const
{
    window_info info;
    info.hwnd = hwnd;
    info.valid = is_valid(hwnd);
    if (!info.valid) return info;

    info.pid = get_window_pid(hwnd);
    info.visible = is_visible(hwnd);
    info.ownerless = is_ownerless_top_level(hwnd);
    info.style = get_style(hwnd);
    info.ex_style = get_ex_style(hwnd);

    RECT rc{};
    if (get_effective_window_rect(hwnd, rc)) {
        info.rect_ok = true;
        info.rect_screen = rc;
    }

    info.title = get_window_text_utf8(hwnd);
    info.class_name = get_window_class_utf8(hwnd);
    return info;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          枚举指定 PID 的可见顶层窗口（用于 surfaces/capture）
/// @参数
///          pid--目标进程 PID
/// @返回值
///          windows--窗口快照列表（按 Z 递增编号）
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
std::vector<window_ops::window_info> window_ops::enumerate_visible_top_level(DWORD pid) const
{
    std::vector<window_info> out;
    if (pid == 0) return out;

    struct ctx {
        DWORD target_pid = 0;
        std::vector<window_info>* windows = nullptr;
        int next_z = 0;
        const window_ops* ops = nullptr;
    } c{pid, &out, 0, this};

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<ctx*>(lp);
            if (!c || !c->windows || !c->ops) return TRUE;

            DWORD win_pid = 0;
            GetWindowThreadProcessId(hwnd, &win_pid);
            if (win_pid != c->target_pid) return TRUE;
            if (!IsWindowVisible(hwnd)) return TRUE;

            // Filter input method windows by class name (legacy behavior).
            char class_name[256] = {0};
            GetClassNameA(hwnd, class_name, sizeof(class_name));
            if (std::strcmp(class_name, "IME") == 0) return TRUE;
            if (std::strcmp(class_name, "MSCTFIME UI") == 0) return TRUE;
            if (std::strcmp(class_name, "SoPY_Status") == 0) return TRUE;

            window_info info = c->ops->snapshot(hwnd);
            info.z_order = c->next_z++;
            c->windows->push_back(std::move(info));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&c));

    return out;
}

#else

bool window_ops::is_valid(HWND) const { return false; }
bool window_ops::is_visible(HWND) const { return false; }
bool window_ops::is_ownerless_top_level(HWND) const { return false; }
DWORD window_ops::get_window_pid(HWND) const { return 0; }
LONG_PTR window_ops::get_style(HWND) const { return 0; }
LONG_PTR window_ops::get_ex_style(HWND) const { return 0; }
bool window_ops::get_window_rect(HWND, RECT&) const { return false; }
bool window_ops::get_effective_window_rect(HWND, RECT&) const { return false; }
std::string window_ops::get_window_text_utf8(HWND) const { return {}; }
std::string window_ops::get_window_class_utf8(HWND) const { return {}; }
bool window_ops::query_dwm_attribute_dword(HWND, DWORD, DWORD& out_value) const
{
    out_value = 0;
    return false;
}
std::vector<window_ops::window_info> window_ops::enumerate_visible_top_level(DWORD) const { return {}; }
window_ops::window_info window_ops::snapshot(HWND) const { return window_info{}; }

#endif

