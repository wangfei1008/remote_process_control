#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "common/process_ops.h"

class RemoteProcessSession {
public:
    // === 进程生命周期（委托 process_ops 实现）===
    bool launch_process(const std::string& exe_path,
                        PROCESS_INFORMATION& out_process_info,
                        DWORD& out_launch_pid,
                        DWORD& out_capture_pid,
                        HWND& out_main_window,
                        std::string& out_target_exe_base_name);
    void terminate_processes(PROCESS_INFORMATION& process_info, DWORD capture_pid, DWORD launch_pid);

    // === 窗口选择 ===
    // 枚举指定 PID 下的全部窗口（诊断用，不保证可采集）。
    std::vector<HWND> find_all_windows(DWORD pid) const;
    // 主路径：按 PID 选择最优“可采集主窗”。
    HWND find_main_window(DWORD pid) const;
    // 兜底路径：按进程 basename（如 photoshop.exe）选择最优窗口。
    // anchor_process_root 非 0 时：仅考虑该 PID 及其子进程上的窗口，避免误绑其它同名实例。
    HWND find_window_by_exe_basename(const std::string& exe_base_name,
                                      DWORD anchor_process_root = 0) const;
    // 二级兜底：按标题/类名包含 exe stem（如 photoshop）选择窗口（同样受 anchor_process_root 约束）。
    HWND find_window_by_exe_hint(const std::string& exe_base_name,
                                  DWORD anchor_process_root = 0) const;
    // 判断当前窗口是否仍可作为采集目标（用于健康检查与重选）。
    bool is_window_viable_for_capture(HWND hwnd) const;

    // === 诊断输出 ===
    // 输出重绑定候选窗口摘要，帮助定位“匹配失败/评分淘汰”原因。
    void log_window_candidates_for_rebind(DWORD capture_pid, const std::string& target_exe_base_name) const;

    // === 进程查询/工具（供 policy 调用，避免直接散落 Win32 调用）===
    bool process_is_running(DWORD pid) const { return m_proc.is_running(pid); }
    std::string get_process_basename(DWORD pid) const { return m_proc.get_process_basename_lower(pid); }
    bool pid_is_same_or_descendant(DWORD pid, DWORD ancestor_pid) const
    {
        return m_proc.is_same_or_descendant(pid, ancestor_pid);
    }

private:
    process_ops m_proc;
};

