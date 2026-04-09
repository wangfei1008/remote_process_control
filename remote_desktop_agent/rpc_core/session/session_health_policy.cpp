#include "session/session_health_policy.h"

#include "session/process_lifecycle.h"

#include "common/rpc_time.h"

#include <iostream>
#include <vector>

HWND SessionHealthPolicy::try_recover_main_window(RemoteProcessSession& remote_process_session,
                                                  DWORD launch_pid,
                                                  DWORD& io_capture_pid,
                                                  const std::string& target_exe_base_name,
                                                  bool allow_pid_rebind_by_exename,
                                                  bool had_successful_video,
                                                  uint64_t pid_rebind_deadline_unix_ms)
{
    // 主路径：优先按当前 capture_pid 找主窗。
    HWND main_window = remote_process_session.find_main_window(io_capture_pid);

    if (allow_pid_rebind_by_exename &&
        !had_successful_video &&
        !main_window &&
        !target_exe_base_name.empty() &&
        rpc_unix_epoch_ms() <= pid_rebind_deadline_unix_ms &&
        launch_pid != 0 &&
        process_lifecycle::process_is_running(launch_pid)) {
        // 启动窗口期兜底：按 basename 匹配窗口并允许 PID 重绑定。
        HWND hwnd = remote_process_session.find_window_by_exe_basename(target_exe_base_name, launch_pid);
        bool by_hint = false;
        if (!hwnd) {
            // 二级匹配：当无法按进程路径精确匹配时，退回到“窗口标题/类名包含 exe stem”。
            // 主要用于某些应用启动阶段 PID/进程镜像查询不稳定的场景（如 Photoshop 多进程启动链）。
            hwnd = remote_process_session.find_window_by_exe_hint(target_exe_base_name, launch_pid);
            by_hint = (hwnd != nullptr);
        }
        if (hwnd) {
            DWORD real_pid = 0;
            GetWindowThreadProcessId(hwnd, &real_pid);
            if (real_pid) {
                // 记录 PID 重绑定，便于排查多进程启动链（如 Photoshop）。
                if (real_pid != io_capture_pid) {
                    std::cout << "[proc] pid rebound by exe, old_pid=" << io_capture_pid
                              << " new_pid=" << real_pid
                              << " exe=" << target_exe_base_name << std::endl;
                }
                if (by_hint) {
                    std::cout << "[proc] window rebound by exe-hint, pid=" << real_pid
                              << " exe=" << target_exe_base_name << std::endl;
                }
                io_capture_pid = real_pid;
                main_window = hwnd;
            }
        }
    }
    return main_window;
}

void SessionHealthPolicy::maybe_log_no_window(RemoteProcessSession& remote_process_session,
                                              DWORD capture_pid,
                                              const std::string& target_exe_base_name,
                                              std::chrono::steady_clock::time_point& io_last_no_window_diag)
{
    const auto now = std::chrono::steady_clock::now();
    // 节流到 1s，避免“无窗口期”高频重复日志。
    if (now - io_last_no_window_diag <= std::chrono::seconds(1)) return;
    io_last_no_window_diag = now;
    std::vector<HWND> windows = remote_process_session.find_all_windows(capture_pid);
    std::cout << "[proc] no window yet, pid=" << capture_pid
              << " windows=" << windows.size() << std::endl;
    // 每秒输出一次候选窗口摘要，帮助定位“无窗口/匹配失败/评分淘汰”具体卡在哪个环节。
    remote_process_session.log_window_candidates_for_rebind(capture_pid, target_exe_base_name);
}

bool SessionHealthPolicy::should_notify_remote_exit(bool had_successful_video,
                                                    uint64_t now_ms,
                                                    uint64_t& io_window_missing_since_unix_ms,
                                                    uint32_t window_missing_exit_grace_ms)
{
    if (io_window_missing_since_unix_ms == 0) {
        io_window_missing_since_unix_ms = now_ms;
    }
    if (!had_successful_video || now_ms < io_window_missing_since_unix_ms) return false;
    return (now_ms - io_window_missing_since_unix_ms) >= static_cast<uint64_t>(window_missing_exit_grace_ms);
}

