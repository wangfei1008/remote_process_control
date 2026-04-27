// ============================================================
// gdi_capture_backend.cpp
// GDI 采集后端实现
//
// 原代码问题：gdi_capture_window_rgb24 有多条早返回路径，
// 每条都需要手工记住该释放哪些资源，容易遗漏。
// 本版本用 RAII Guard 封装所有 GDI 资源，任意路径均安全。
// ============================================================

#include "gdi_capture_backend.h"
#include "../infra/win32_window.h"

#if defined(_WIN32)
#include "../infra/gdi_resource_guards.h"
#include <algorithm>
#include <cstring>
#include <vector>

#ifndef PW_CLIENTONLY
#  define PW_CLIENTONLY 0x00000001
#endif

namespace capture {
namespace {

// ----------------------------------------------------------
// 单窗口 GDI 采集，返回 RGB24
// include_non_client: true  → 含标题栏/边框
//                     false → 仅客户区
// ----------------------------------------------------------
std::vector<uint8_t> gdi_capture_rgb24(HWND hwnd, int w, int h,
                                        bool include_non_client)
{
    if (!hwnd || w <= 0 || h <= 0) return {};

    win32::DcGuard      wnd_dc(hwnd, include_non_client);
    if (!wnd_dc) return {};

    win32::CompatDcGuard mem_dc(wnd_dc.get());
    if (!mem_dc) return {};

    win32::BitmapGuard  bmp(mem_dc.get(), w, h, wnd_dc.get());
    if (!bmp) return {};

    // ---- 绘制 -----------------------------------------------
    BOOL ok = FALSE;

    if (include_non_client) {
        // 优先：直接从屏幕 BitBlt（含 DWM 合成效果）
        win32::Window wops;
        RECT wr{};
        if (wops.get_effective_rect(hwnd, wr)) {
            win32::DcGuard screen_dc;   // GetDC(nullptr)
            if (screen_dc) {
                ok = BitBlt(mem_dc.get(), 0, 0, w, h,
                            screen_dc.get(), wr.left, wr.top,
                            SRCCOPY | CAPTUREBLT);
            }
        }
        // 回退 1：从窗口 DC BitBlt
        if (!ok)
            ok = BitBlt(mem_dc.get(), 0, 0, w, h,
                        wnd_dc.get(), 0, 0, SRCCOPY | CAPTUREBLT);
        // 回退 2：PrintWindow
        if (!ok)
            ok = PrintWindow(hwnd, mem_dc.get(), 0);
    } else {
        ok = PrintWindow(hwnd, mem_dc.get(), PW_CLIENTONLY);
        if (!ok)
            ok = BitBlt(mem_dc.get(), 0, 0, w, h,
                        wnd_dc.get(), 0, 0, SRCCOPY | CAPTUREBLT);
    }

    if (!ok) return {};

    // ---- 读取像素 -------------------------------------------
    BITMAPINFOHEADER bi{};
    bi.biSize        = sizeof(bi);
    bi.biWidth       = w;
    bi.biHeight      = -h;   // top-down
    bi.biPlanes      = 1;
    bi.biBitCount    = 32;
    bi.biCompression = BI_RGB;

    std::vector<uint8_t> raw(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    const int lines = GetDIBits(mem_dc.get(), bmp.get(), 0, h,
                                 raw.data(),
                                 reinterpret_cast<BITMAPINFO*>(&bi),
                                 DIB_RGB_COLORS);
    if (lines != h) return {};

    // ---- BGRA → RGB24 ---------------------------------------
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);
    for (int i = 0, j = 0; i < w * h * 4; i += 4, j += 3) {
        rgb[static_cast<size_t>(j)+0] = raw[static_cast<size_t>(i)+2]; // R
        rgb[static_cast<size_t>(j)+1] = raw[static_cast<size_t>(i)+1]; // G
        rgb[static_cast<size_t>(j)+2] = raw[static_cast<size_t>(i)+0]; // B
    }
    return rgb;
}

// ----------------------------------------------------------
// 采集单个窗口到 WindowTile（不含 z_order，由调用方填）
// ----------------------------------------------------------
bool capture_window(const win32::WindowInfo& info, WindowTile& out)
{
    out = WindowTile{};
    out.hwnd        = info.hwnd;
    out.rect_screen = info.rect_screen;

    win32::Window wops;
    if (!wops.is_valid(info.hwnd)) return false;

    RECT capture_rect{};
    if (!wops.get_effective_rect(info.hwnd, capture_rect)) return false;

    const int w = capture_rect.right  - capture_rect.left;
    const int h = capture_rect.bottom - capture_rect.top;
    if (w <= 0 || h <= 0) return false;

    auto frame = gdi_capture_rgb24(info.hwnd, w, h, /*include_non_client=*/true);
    if (frame.empty()) return false;

    // 裁到偶数尺寸（编码器要求）
    const int ew = w & ~1;
    const int eh = h & ~1;
    if (ew <= 0 || eh <= 0) return false;

    if (ew != w || eh != h) {
        // 裁掉右/下边缘各 1 像素
        std::vector<uint8_t> cropped(static_cast<size_t>(ew)*static_cast<size_t>(eh)*3u, 0);
        for (int y = 0; y < eh; ++y) {
            const size_t src_off = static_cast<size_t>(y) * static_cast<size_t>(w)  * 3u;
            const size_t dst_off = static_cast<size_t>(y) * static_cast<size_t>(ew) * 3u;
            std::memcpy(cropped.data() + dst_off,
                        frame.data()  + src_off,
                        static_cast<size_t>(ew) * 3u);
        }
        frame.swap(cropped);
    }

    out.rgb         = std::move(frame);
    out.w           = ew;
    out.h           = eh;
    out.origin_left = capture_rect.left;
    out.origin_top  = capture_rect.top;
    return true;
}

} // namespace

// ---- GdiCaptureBackend 公开接口 -----------------------------

bool GdiCaptureBackend::capture_tiles(
    std::span<const win32::WindowInfo> surfaces,
    std::vector<WindowTile>&           out_tiles,
    uint64_t /*now_unix_ms*/)
{
    out_tiles.clear();
    if (surfaces.empty()) return false;

    out_tiles.reserve(surfaces.size());
    for (const auto& s : surfaces) {
        WindowTile t{};
        t.z_order = s.z_order;
        if (!capture_window(s, t)) return false;

        const size_t expected =
            static_cast<size_t>(t.w) * static_cast<size_t>(t.h) * 3u;
        if (t.rgb.size() != expected) return false;

        out_tiles.push_back(std::move(t));
    }
    return true;
}

} // namespace capture

#else // !_WIN32

namespace capture {
bool GdiCaptureBackend::capture_tiles(std::span<const win32::WindowInfo>,
                                       std::vector<WindowTile>&, uint64_t)
{ return false; }
} // namespace capture

#endif
