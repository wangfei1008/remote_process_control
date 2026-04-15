#include "process_ops.h"

#if defined(_WIN32)
#include <TlHelp32.h>

#include <algorithm>
#include <cctype>

namespace {
} // namespace

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          析构：按会话语义自动 stop（默认终止进程并释放句柄）
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
process_ops::~process_ops()
{
    if (m_stop_on_destruct) {
        stop(true, true, 0);
    } else {
        m_pi.reset();
    }
}

std::string process_ops::basename_from_path(const std::string& path)
{
    const auto pos = path.find_last_of("\\/");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

std::string process_ops::to_lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          会话级启动：CreateProcess 并保存 launch/capture PID 与 exe basename（lower）
/// @参数
///          exe_path--可执行文件路径
///          creation_flags--CreateProcess flags
///          show_maximized--是否最大化显示（与 legacy 行为对齐）
/// @返回值
///          true--启动成功并持有句柄
///          false--启动失败
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
bool process_ops::start(const std::string& exe_path, DWORD creation_flags, bool show_maximized)
{
    stop(false, true, 0);

    m_exe_path = exe_path;
    m_target_exe_base_name_lower = to_lower_ascii(basename_from_path(exe_path));

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (show_maximized) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMAXIMIZED;
    }

    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(exe_path.c_str(), nullptr, nullptr, nullptr, FALSE, creation_flags, nullptr, nullptr, &si,
                        &pi)) {
        m_launch_pid = 0;
        m_capture_pid = 0;
        return false;
    }

    m_pi.get() = pi;
    m_launch_pid = pi.dwProcessId;
    m_capture_pid = m_launch_pid;
    return true;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          会话级停止：可选终止 launch 进程与 capture 子进程，并释放句柄
/// @参数
///          terminate_launch_process--是否终止 launch 进程
///          terminate_capture_process_if_child--capture_pid != launch_pid 时是否终止 capture
///          exit_code--TerminateProcess exit code
/// @返回值
///          无
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
void process_ops::stop(bool terminate_launch_process, bool terminate_capture_process_if_child, UINT exit_code)
{
    const DWORD launch_pid = m_launch_pid;
    const DWORD capture_pid = m_capture_pid;

    if (terminate_launch_process && m_pi.get().hProcess) {
        terminate_by_handle(m_pi.get().hProcess, exit_code);
    }

    m_pi.reset();
    m_launch_pid = 0;
    m_capture_pid = 0;
    m_target_exe_base_name_lower.clear();
    m_exe_path.clear();

    if (terminate_capture_process_if_child && capture_pid != 0 && capture_pid != launch_pid) {
        terminate_by_pid(capture_pid, exit_code);
    }
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          判断本会话 launch 进程是否仍存活（基于句柄 exit_code）
/// @参数
///          无
/// @返回值
///          true--仍存活
///          false--已退出或无句柄
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
bool process_ops::running() const
{
    if (!m_pi.get().hProcess) return false;
    DWORD code = 0;
    if (GetExitCodeProcess(m_pi.get().hProcess, &code) == FALSE) return false;
    return code == STILL_ACTIVE;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          转移 launch 进程句柄所有权到 out_process_info（避免双重 CloseHandle）
/// @参数
///          out_process_info--输出 PROCESS_INFORMATION（接管 hProcess/hThread）
/// @返回值
///          无
///
/// @时间    2026/4/15
/////////////////////////////////////////////////////////////////////////////
void process_ops::detach_launch_process_info(PROCESS_INFORMATION& out_process_info)
{
    out_process_info = m_pi.get();
    // 仅清空句柄，避免析构/stop 误 CloseHandle；pid 等语义状态保留。
    m_pi.get() = PROCESS_INFORMATION{};
}

process_ops::scoped_handle process_ops::open_process(DWORD pid, DWORD access) const
{
    if (pid == 0) return scoped_handle{};
    HANDLE h = OpenProcess(access, FALSE, pid);
    return scoped_handle(h);
}

bool process_ops::query_full_image_name(HANDLE process_handle, std::string& out_path) const
{
    out_path.clear();
    if (!process_handle) return false;

    char buf[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameA(process_handle, 0, buf, &size)) return false;

    out_path.assign(buf, buf + size);
    return true;
}

bool process_ops::query_full_image_name(DWORD pid, std::string& out_path) const
{
    auto h = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (!h) return false;
    return query_full_image_name(h.get(), out_path);
}

std::string process_ops::get_process_basename_lower(DWORD pid) const
{
    std::string path;
    if (!query_full_image_name(pid, path)) return {};
    return to_lower_ascii(basename_from_path(path));
}

bool process_ops::get_exit_code(HANDLE process_handle, DWORD& out_exit_code) const
{
    out_exit_code = 0;
    if (!process_handle) return false;
    return GetExitCodeProcess(process_handle, &out_exit_code) != FALSE;
}

bool process_ops::is_running(DWORD pid) const
{
    if (pid == 0) return false;
    auto h = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION);
    if (!h) return false;

    DWORD code = 0;
    if (GetExitCodeProcess(h.get(), &code) == FALSE) return false;
    return code == STILL_ACTIVE;
}

bool process_ops::terminate_by_handle(HANDLE process_handle, UINT exit_code) const
{
    if (!process_handle) return false;
    return TerminateProcess(process_handle, exit_code) != FALSE;
}

bool process_ops::terminate_by_pid(DWORD pid, UINT exit_code) const
{
    if (pid == 0) return false;
    auto h = open_process(pid, PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION);
    if (!h) return false;
    return terminate_by_handle(h.get(), exit_code);
}

DWORD process_ops::parent_pid_toolhelp(DWORD pid) const
{
    if (pid == 0) return 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    DWORD parent = 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                parent = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return parent;
}

bool process_ops::is_same_or_descendant(DWORD pid, DWORD ancestor_pid, int max_depth) const
{
    if (pid == 0 || ancestor_pid == 0) return false;
    if (pid == ancestor_pid) return true;
    if (max_depth <= 0) return false;

    DWORD current = pid;
    for (int depth = 0; depth < max_depth; ++depth) {
        const DWORD parent = parent_pid_toolhelp(current);
        if (parent == 0) return false;
        if (parent == ancestor_pid) return true;
        current = parent;
    }
    return false;
}

#else

process_ops::~process_ops() {}

bool process_ops::start(const std::string&, DWORD, bool) { return false; }
void process_ops::stop(bool, bool, UINT) {}
bool process_ops::running() const { return false; }

process_ops::scoped_handle process_ops::open_process(DWORD, DWORD) const { return scoped_handle{}; }
bool process_ops::query_full_image_name(HANDLE, std::string& out_path) const
{
    out_path.clear();
    return false;
}
bool process_ops::query_full_image_name(DWORD, std::string& out_path) const
{
    out_path.clear();
    return false;
}
std::string process_ops::get_process_basename_lower(DWORD) const { return {}; }
bool process_ops::get_exit_code(HANDLE, DWORD& out_exit_code) const
{
    out_exit_code = 0;
    return false;
}
bool process_ops::is_running(DWORD) const { return false; }
bool process_ops::terminate_by_handle(HANDLE, UINT) const { return false; }
bool process_ops::terminate_by_pid(DWORD, UINT) const { return false; }
DWORD process_ops::parent_pid_toolhelp(DWORD) const { return 0; }
bool process_ops::is_same_or_descendant(DWORD, DWORD, int) const { return false; }

std::string process_ops::basename_from_path(const std::string& path) { return path; }
std::string process_ops::to_lower_ascii(std::string s) { return s; }

#endif

