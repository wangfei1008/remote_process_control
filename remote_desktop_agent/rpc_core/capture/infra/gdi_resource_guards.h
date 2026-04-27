#pragma once

// ============================================================
// gdi_resource_guards.h
// GDI 资源 RAII 守卫
//
// 职责：确保任意返回路径下 GDI 资源都被正确释放
// 使用方：gdi_capture_backend.cpp（内部实现文件，不对外暴露）
// ============================================================

#include "win32_types.h"

namespace win32 {

// ----------------------------------------------------------
// DcGuard：设备上下文 RAII
// ----------------------------------------------------------
class DcGuard {
public:
    // 窗口 DC（非客户区 = GetWindowDC，客户区 = GetDC）
    DcGuard(HWND hwnd, bool non_client)
        : m_hwnd(hwnd)
        , m_hdc(non_client ? GetWindowDC(hwnd) : GetDC(hwnd))
        , m_is_screen(false)
    {}

    // 屏幕 DC（GetDC(nullptr)）
    DcGuard()
        : m_hwnd(nullptr)
        , m_hdc(GetDC(nullptr))
        , m_is_screen(true)
    {}

    ~DcGuard() {
        if (m_hdc) ReleaseDC(m_is_screen ? nullptr : m_hwnd, m_hdc);
    }

    DcGuard(const DcGuard&)            = delete;
    DcGuard& operator=(const DcGuard&) = delete;

    HDC  get()  const { return m_hdc; }
    explicit operator bool() const { return m_hdc != nullptr; }

private:
    HWND m_hwnd;
    HDC  m_hdc;
    bool m_is_screen;
};

// ----------------------------------------------------------
// CompatDcGuard：兼容 DC（CreateCompatibleDC）RAII
// ----------------------------------------------------------
class CompatDcGuard {
public:
    explicit CompatDcGuard(HDC ref)
        : m_hdc(CreateCompatibleDC(ref)) {}

    ~CompatDcGuard() {
        if (m_hdc) DeleteDC(m_hdc);
    }

    CompatDcGuard(const CompatDcGuard&)            = delete;
    CompatDcGuard& operator=(const CompatDcGuard&) = delete;

    HDC  get()  const { return m_hdc; }
    explicit operator bool() const { return m_hdc != nullptr; }

private:
    HDC m_hdc;
};

// ----------------------------------------------------------
// BitmapGuard：兼容位图 + SelectObject RAII
// ----------------------------------------------------------
class BitmapGuard {
public:
    BitmapGuard(HDC mem_dc, int w, int h, HDC ref_dc)
        : m_mem_dc(mem_dc)
        , m_hbm(CreateCompatibleBitmap(ref_dc, w, h))
        , m_prev(m_hbm ? SelectObject(mem_dc, m_hbm) : nullptr)
    {}

    ~BitmapGuard() {
        if (m_prev) SelectObject(m_mem_dc, m_prev);
        if (m_hbm)  DeleteObject(m_hbm);
    }

    BitmapGuard(const BitmapGuard&)            = delete;
    BitmapGuard& operator=(const BitmapGuard&) = delete;

    HBITMAP get()  const { return m_hbm; }
    explicit operator bool() const { return m_hbm != nullptr; }

private:
    HDC     m_mem_dc;
    HBITMAP m_hbm;
    HGDIOBJ m_prev;
};

} // namespace win32