#include "session/process_lifecycle.h"

#include "session/window_selection_utils.h"

#include <iostream>

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

bool launch_process(const std::string& exe_path,
                    PROCESS_INFORMATION& out_process_info,
                    DWORD& out_launch_pid,
                    DWORD& out_capture_pid,
                    std::string& out_target_exe_base_name)
{
    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(startup_info);

    out_target_exe_base_name = window_selection_utils::to_lower_ascii(basename_from_path(exe_path));

    if (!CreateProcessA(exe_path.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startup_info, &out_process_info)) {
        std::cout << "[proc] CreateProcess failed, exe=" << exe_path
                  << " error=" << GetLastError() << std::endl;
        return false;
    }

    std::cout << "[proc] CreateProcess ok, pid=" << out_process_info.dwProcessId
              << " exe=" << exe_path << std::endl;

    out_launch_pid = out_process_info.dwProcessId;
    out_capture_pid = out_launch_pid;
    return true;
}

void terminate_processes(PROCESS_INFORMATION& process_info, DWORD capture_pid, DWORD launch_pid)
{
    if (process_info.hProcess) {
        TerminateProcess(process_info.hProcess, 0);
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
            TerminateProcess(process_handle, 0);
            CloseHandle(process_handle);
        }
    }
}

} // namespace process_lifecycle

