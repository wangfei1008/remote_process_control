#include "session/session_health_policy.h"

#include "common/rpc_time.h"

#include <iostream>
#include <vector>

HWND SessionHealthPolicy::try_recover_main_window(RemoteProcessSession& remote_process_session,
                                                  DWORD& io_capture_pid,
                                                  const std::string& target_exe_base_name,
                                                  bool allow_pid_rebind_by_exename,
                                                  bool had_successful_video,
                                                  uint64_t pid_rebind_deadline_unix_ms)
{
    HWND main_window = remote_process_session.find_main_window(io_capture_pid);
    if (!main_window) {
        std::vector<HWND> windows = remote_process_session.find_all_windows(io_capture_pid);
        for (HWND hwnd : windows) {
            if (IsWindowVisible(hwnd)) {
                main_window = hwnd;
                break;
            }
        }
        if (!main_window && !windows.empty()) {
            main_window = windows.front();
        }
    }

    if (allow_pid_rebind_by_exename &&
        !had_successful_video &&
        !main_window &&
        !target_exe_base_name.empty() &&
        rpc_unix_epoch_ms() <= pid_rebind_deadline_unix_ms) {
        HWND hwnd = remote_process_session.find_window_by_exe_basename(target_exe_base_name);
        if (hwnd) {
            DWORD real_pid = 0;
            GetWindowThreadProcessId(hwnd, &real_pid);
            if (real_pid) {
                io_capture_pid = real_pid;
                main_window = hwnd;
            }
        }
    }
    return main_window;
}

void SessionHealthPolicy::maybe_log_no_window(RemoteProcessSession& remote_process_session,
                                              DWORD capture_pid,
                                              std::chrono::steady_clock::time_point& io_last_no_window_diag)
{
    const auto now = std::chrono::steady_clock::now();
    if (now - io_last_no_window_diag <= std::chrono::seconds(1)) return;
    io_last_no_window_diag = now;
    std::vector<HWND> windows = remote_process_session.find_all_windows(capture_pid);
    std::cout << "[proc] no window yet, pid=" << capture_pid
              << " windows=" << windows.size() << std::endl;
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

