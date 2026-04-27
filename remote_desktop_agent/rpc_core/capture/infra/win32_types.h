#pragma once

// ============================================================
// win32_types.h
// Win32 HANDLE RAII + 跨平台占位定义
// 规则：本文件只做类型/守卫，不含任何业务逻辑
// ============================================================

#if defined(_WIN32)
#  include <windows.h>
#else
// ---- 非 Windows：clangd 索引用占位，不参与实际运行 ----
using HANDLE   = void*;
using DWORD    = unsigned long;
using UINT     = unsigned int;
using LONG_PTR = long long;
using BOOL     = int;
using HWND     = void*;
using HDC      = void*;
using HBITMAP  = void*;
using HGDIOBJ  = void*;
struct RECT { long left=0, top=0, right=0, bottom=0; };
struct PROCESS_INFORMATION {
    HANDLE hProcess = nullptr;
    HANDLE hThread  = nullptr;
    DWORD  dwProcessId = 0;
    DWORD  dwThreadId  = 0;
};
inline BOOL CloseHandle(HANDLE)            { return 0; }
inline BOOL ReleaseDC(HWND, HDC)           { return 0; }
inline BOOL DeleteDC(HDC)                  { return 0; }
inline BOOL DeleteObject(HGDIOBJ)          { return 0; }
static constexpr DWORD PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
static constexpr DWORD PROCESS_TERMINATE                 = 0x0001;
static constexpr DWORD STILL_ACTIVE                      = 259;
#endif

#include <utility>

namespace win32 {

// ----------------------------------------------------------
// ScopedHandle：HANDLE RAII 包装
// ----------------------------------------------------------
class ScopedHandle {
public:
    ScopedHandle() = default;

    explicit ScopedHandle(HANDLE h) : m_h(h) {}

    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&)            = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& o) noexcept : m_h(o.m_h) { o.m_h = nullptr; }
    ScopedHandle& operator=(ScopedHandle&& o) noexcept {
        if (this != &o) { reset(); m_h = o.m_h; o.m_h = nullptr; }
        return *this;
    }

    HANDLE get()     const { return m_h; }
    explicit operator bool() const {
#if defined(_WIN32)
        return m_h != nullptr && m_h != INVALID_HANDLE_VALUE;
#else
        return m_h != nullptr;
#endif
    }

    HANDLE release() { HANDLE t = m_h; m_h = nullptr; return t; }

    void reset(HANDLE h = nullptr) {
#if defined(_WIN32)
        if (m_h && m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h);
#else
        if (m_h) CloseHandle(m_h);
#endif
        m_h = h;
    }

private:
    HANDLE m_h = nullptr;
};

// ----------------------------------------------------------
// ProcessInfoRaii：PROCESS_INFORMATION RAII 包装
// ----------------------------------------------------------
class ProcessInfoRaii {
public:
    ProcessInfoRaii() = default;
    ~ProcessInfoRaii() { reset(); }

    ProcessInfoRaii(const ProcessInfoRaii&)            = delete;
    ProcessInfoRaii& operator=(const ProcessInfoRaii&) = delete;

    ProcessInfoRaii(ProcessInfoRaii&& o) noexcept : m_pi(o.m_pi) {
        o.m_pi = PROCESS_INFORMATION{};
    }
    ProcessInfoRaii& operator=(ProcessInfoRaii&& o) noexcept {
        if (this != &o) { reset(); m_pi = o.m_pi; o.m_pi = PROCESS_INFORMATION{}; }
        return *this;
    }

    PROCESS_INFORMATION&       get()       { return m_pi; }
    const PROCESS_INFORMATION& get() const { return m_pi; }

    DWORD  pid()            const { return m_pi.dwProcessId; }
    HANDLE process_handle() const { return m_pi.hProcess; }
    HANDLE thread_handle()  const { return m_pi.hThread; }

    void reset() {
        if (m_pi.hProcess) { CloseHandle(m_pi.hProcess); m_pi.hProcess = nullptr; }
        if (m_pi.hThread)  { CloseHandle(m_pi.hThread);  m_pi.hThread  = nullptr; }
        m_pi.dwProcessId = 0;
        m_pi.dwThreadId  = 0;
    }

    // 转移所有权给外部（仅清空句柄字段，pid 保留供诊断）
    PROCESS_INFORMATION detach() {
        PROCESS_INFORMATION out = m_pi;
        m_pi.hProcess    = nullptr;
        m_pi.hThread     = nullptr;
        return out;
    }

private:
    PROCESS_INFORMATION m_pi{};
};

} // namespace win32