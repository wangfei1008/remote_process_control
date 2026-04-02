#pragma once

#include <windows.h>

#include <string>

namespace process_lifecycle {

// 仅负责进程启动与目标 exe 名解析，不处理窗口选择。
bool launch_process(const std::string& exe_path,
                    PROCESS_INFORMATION& out_process_info,
                    DWORD& out_launch_pid,
                    DWORD& out_capture_pid,
                    std::string& out_target_exe_base_name);

// 仅负责进程句柄/进程终止，不处理窗口状态。
void terminate_processes(PROCESS_INFORMATION& process_info, DWORD capture_pid, DWORD launch_pid);

// 按 PID 读取进程镜像文件名（小写 basename），供窗口匹配逻辑使用。
std::string get_process_basename(DWORD pid);

} // namespace process_lifecycle

