#pragma once

#include <windows.h>

#include <string>
#include <vector>

class RemoteProcessSession {
public:
    bool launch_process(const std::string& exe_path,
                        PROCESS_INFORMATION& out_process_info,
                        DWORD& out_launch_pid,
                        DWORD& out_capture_pid,
                        HWND& out_main_window,
                        std::string& out_target_exe_base_name);
    void terminate_processes(PROCESS_INFORMATION& process_info, DWORD capture_pid, DWORD launch_pid);

    std::vector<HWND> find_all_windows(DWORD pid) const;
    HWND find_main_window(DWORD pid) const;
    HWND find_window_by_exe_basename(const std::string& exe_base_name) const;

private:
    static std::string basename_from_path(const std::string& path);
    static std::string get_process_basename(DWORD pid);
};

