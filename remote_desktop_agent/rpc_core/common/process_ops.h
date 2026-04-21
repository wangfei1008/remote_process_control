#pragma once

#if defined(_WIN32)
#include <windows.h>
#else
// 非 Windows 平台下用于 clangd 索引的最小占位定义（不参与实际运行）。
using HANDLE = void*;
using DWORD = unsigned long;
using UINT = unsigned int;
using LONG_PTR = long long;
static constexpr DWORD PROCESS_QUERY_LIMITED_INFORMATION = 0;
inline int CloseHandle(HANDLE) { return 0; }
struct PROCESS_INFORMATION {
    HANDLE hProcess = nullptr;
    HANDLE hThread = nullptr;
    DWORD dwProcessId = 0;
    DWORD dwThreadId = 0;
};
#endif

#include <string>
#include "character_conversion.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： Win32 进程操作封装（会话级注入 + 句柄 RAII + 进程信息查询）
//
// 作者：WangFei
// 时间： 2026-04-15
// 修改:
//              1、2026-04-15创建
//
//详细功能说明：
//- 负责 CreateProcess/OpenProcess/QueryFullProcessImageName/GetExitCodeProcess/TerminateProcess
//- 提供 Toolhelp 父链查询与 is_same_or_descendant
//- 会话级持有 launch/capture PID 与 PROCESS_INFORMATION，析构自动释放句柄（可选终止）
//- 不包含窗口评分/窗口选择策略/界面枚举（这些属于策略层与采集层）
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

class process_ops {
public:
    class scoped_handle {
    public:
        scoped_handle() = default;
        explicit scoped_handle(HANDLE h) : m_h(h) {}
        ~scoped_handle() { reset(); }

        scoped_handle(const scoped_handle&) = delete;
        scoped_handle& operator=(const scoped_handle&) = delete;

        scoped_handle(scoped_handle&& other) noexcept : m_h(other.m_h) { other.m_h = nullptr; }
        scoped_handle& operator=(scoped_handle&& other) noexcept
        {
            if (this == &other) return *this;
            reset();
            m_h = other.m_h;
            other.m_h = nullptr;
            return *this;
        }

        HANDLE get() const { return m_h; }
        explicit operator bool() const
        {
#if defined(_WIN32)
            return m_h != nullptr && m_h != INVALID_HANDLE_VALUE;
#else
            return m_h != nullptr;
#endif
        }

        HANDLE release()
        {
            HANDLE tmp = m_h;
            m_h = nullptr;
            return tmp;
        }

        void reset(HANDLE h = nullptr)
        {
#if defined(_WIN32)
            if (m_h && m_h != INVALID_HANDLE_VALUE) {
                CloseHandle(m_h);
            }
#else
            if (m_h) {
                CloseHandle(m_h);
            }
#endif
            m_h = h;
        }

    private:
        HANDLE m_h = nullptr;
    };

    class process_info {
    public:
        process_info() = default;
        ~process_info() { reset(); }

        process_info(const process_info&) = delete;
        process_info& operator=(const process_info&) = delete;

        process_info(process_info&& other) noexcept : m_pi(other.m_pi)
        {
            other.m_pi = PROCESS_INFORMATION{};
        }
        process_info& operator=(process_info&& other) noexcept
        {
            if (this == &other) return *this;
            reset();
            m_pi = other.m_pi;
            other.m_pi = PROCESS_INFORMATION{};
            return *this;
        }

        PROCESS_INFORMATION& get() { return m_pi; }
        const PROCESS_INFORMATION& get() const { return m_pi; }
        DWORD pid() const { return m_pi.dwProcessId; }
        HANDLE process_handle() const { return m_pi.hProcess; }
        HANDLE thread_handle() const { return m_pi.hThread; }

        void reset()
        {
            if (m_pi.hProcess) {
                CloseHandle(m_pi.hProcess);
                m_pi.hProcess = nullptr;
            }
            if (m_pi.hThread) {
                CloseHandle(m_pi.hThread);
                m_pi.hThread = nullptr;
            }
            m_pi.dwProcessId = 0;
            m_pi.dwThreadId = 0;
        }

    private:
        PROCESS_INFORMATION m_pi{};
    };

public:
    process_ops() = default;
    process_ops(const std::string& exe_path, DWORD creation_flags = 0, bool show_maximized = true);
    ~process_ops();

    process_ops(const process_ops&) = delete;
    process_ops& operator=(const process_ops&) = delete;

    process_ops(process_ops&&) = default;
    process_ops& operator=(process_ops&&) = default;

    // === 会话级操作 ===
    bool start();
    void stop(bool terminate_launch_process = true, bool terminate_capture_process_if_child = true, UINT exit_code = 0);
    bool running() const;

    DWORD launch_pid() const { return m_launch_pid; }
    DWORD capture_pid() const { return m_capture_pid; }
    void set_capture_pid(DWORD pid) { m_capture_pid = pid; }
    const std::string& target_exe_base_name_lower() const { return m_target_exe_base_name_lower; }
    const std::string& exe_path() const { return m_exe_path; }
    const process_info& launch_process_info() const { return m_pi; }
    // 将 CreateProcess 返回的 hProcess/hThread 所有权转移给调用方（避免重复 CloseHandle）。
    // 转移后本对象仍保留 launch/capture pid 与 exe basename（用于后续按 pid 查询/终止）。
    void detach_launch_process_info(PROCESS_INFORMATION& out_process_info);

    /// 终止并关闭已由 detach 交给调用方的 launch 句柄；若 capture_pid 与 launch 不同则按 PID 终止 capture。
    void terminate_detached_launch(PROCESS_INFORMATION& process_info,
                                   DWORD capture_pid,
                                   DWORD launch_pid,
                                   UINT exit_code = 0) const;

    // === 进程查询/工具（无策略）===
    scoped_handle open_process(DWORD pid, DWORD access = PROCESS_QUERY_LIMITED_INFORMATION) const;
    bool query_full_image_name(HANDLE process_handle, std::string& out_path) const;
    bool query_full_image_name(DWORD pid, std::string& out_path) const;

    // lowercase ASCII basename (e.g. "Photoshop.exe" -> "photoshop.exe"). Empty on failure.
    std::string get_process_basename_lower(DWORD pid) const;

    bool get_exit_code(HANDLE process_handle, DWORD& out_exit_code) const;
    bool is_running(DWORD pid) const;

    bool terminate_by_handle(HANDLE process_handle, UINT exit_code = 0) const;
    bool terminate_by_pid(DWORD pid, UINT exit_code = 0) const;

    DWORD parent_pid_toolhelp(DWORD pid) const;
    bool is_same_or_descendant(DWORD pid, DWORD ancestor_pid, int max_depth = 64) const;

private:
    static std::string basename_from_path(const std::string& path);

private:
    std::string m_exe_path;
    DWORD m_creation_flags;
    bool m_show_maximized;

    process_info m_pi;
    DWORD m_launch_pid = 0;
    DWORD m_capture_pid = 0;
    std::string m_target_exe_base_name_lower;
    bool m_stop_on_destruct = true;
};

// 兼容外部命名习惯（如注入 ProcessOps 对象）。
using ProcessOps = process_ops;

