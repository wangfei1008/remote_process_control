#pragma once

#include <windows.h>

namespace window_rect_utils {

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

using DwmGetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);

inline DwmGetWindowAttributeFn get_dwm_get_window_attribute_fn()
{
    static DwmGetWindowAttributeFn s_fn = []() -> DwmGetWindowAttributeFn {
        HMODULE module = LoadLibraryA("dwmapi.dll");
        if (!module) return nullptr;
        auto fn = reinterpret_cast<DwmGetWindowAttributeFn>(
            GetProcAddress(module, "DwmGetWindowAttribute"));
        return fn;
    }();
    return s_fn;
}

inline bool get_effective_window_rect(HWND hwnd, RECT& out_rect)
{
    if (!hwnd || !IsWindow(hwnd)) return false;

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return false;

    if (auto fn = get_dwm_get_window_attribute_fn()) {
        RECT ext_rect{};
        if (SUCCEEDED(fn(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &ext_rect, sizeof(ext_rect))) &&
            ext_rect.right > ext_rect.left &&
            ext_rect.bottom > ext_rect.top) {
            rect = ext_rect;
        }
    }

    out_rect = rect;
    return true;
}

} 

