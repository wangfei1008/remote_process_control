#include "session/process_lifecycle.h"

#include "session/window_selection_utils.h"

#include <iostream>
#include <TlHelp32.h>

namespace process_lifecycle {

static std::string basename_from_path(const std::string& path)
{
    const auto pos = path.find_last_of("\\/");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

std::string get_process_basename(DWORD pid)
{
    HANDLE process_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process_handle) return {};

    char path_buffer[MAX_PATH] = {0};
    DWORD path_size = MAX_PATH;
    std::string process_name;
    if (QueryFullProcessImageNameA(process_handle, 0, path_buffer, &path_size)) {
        process_name = window_selection_utils::to_lower_ascii(
            basename_from_path(std::string(path_buffer, path_buffer + path_size)));
    }
    CloseHandle(process_handle);
    return process_name;
}

bool process_is_running(DWORD pid)
{
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

static DWORD toolhelp_parent_process_id(DWORD pid)
{
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

bool pid_is_same_or_descendant(DWORD window_pid, DWORD ancestor_pid)
{
    if (window_pid == 0 || ancestor_pid == 0) return false;
    if (window_pid == ancestor_pid) return true;
    DWORD current = window_pid;
    for (int depth = 0; depth < 64; ++depth) {
        const DWORD parent = toolhelp_parent_process_id(current);
        if (parent == 0) return false;
        if (parent == ancestor_pid) return true;
        current = parent;
    }
    return false;
}

bool launch_process(const std::string& exe_path,
                    PROCESS_INFORMATION& out_process_info,
                    DWORD& out_launch_pid,
                    DWORD& out_capture_pid,
                    std::string& out_target_exe_base_name)
{
    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(startup_info);
	startup_info.dwFlags = STARTF_USESHOWWINDOW;
	startup_info.wShowWindow = SW_SHOWMAXIMIZED;

    out_target_exe_base_name = window_selection_utils::to_lower_ascii(basename_from_path(exe_path));

    if (!CreateProcessA(exe_path.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startup_info, &out_process_info)) {
        std::cout << "[proc] CreateProcess failed, exe=" << exe_path << " error=" << GetLastError() << std::endl;
        return false;
    }

    std::cout << "[proc] CreateProcess ok, pid=" << out_process_info.dwProcessId << " exe=" << exe_path << std::endl;

    out_launch_pid = out_process_info.dwProcessId;
    out_capture_pid = out_launch_pid;
    return true;
}

void terminate_processes(PROCESS_INFORMATION& process_info, DWORD capture_pid, DWORD launch_pid)
{
    if (process_info.hProcess) {
        const DWORD pid = process_info.dwProcessId;
        const BOOL ok = TerminateProcess(process_info.hProcess, 0);
        const DWORD err = ok ? 0 : GetLastError();
        std::cout << "[proc] TerminateProcess launch_pid=" << pid << " ok=" << (ok ? 1 : 0)
                  << " err=" << err << std::endl;
        CloseHandle(process_info.hProcess);
        process_info.hProcess = nullptr;
    }
    if (process_info.hThread) {
        CloseHandle(process_info.hThread);
        process_info.hThread = nullptr;
    }

    if (capture_pid != 0 && capture_pid != launch_pid) {
        HANDLE process_handle = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, capture_pid);
        if (process_handle) {
            const BOOL ok = TerminateProcess(process_handle, 0);
            const DWORD err = ok ? 0 : GetLastError();
            std::cout << "[proc] TerminateProcess capture_pid=" << capture_pid << " ok=" << (ok ? 1 : 0)
                      << " err=" << err << std::endl;
            CloseHandle(process_handle);
        } else {
            std::cout << "[proc] OpenProcess(capture_pid) failed pid=" << capture_pid << " err=" << GetLastError() << std::endl;
        }
    }
}

} // namespace process_lifecycle

