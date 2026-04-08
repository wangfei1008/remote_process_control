#pragma once

#include <windows.h>

#include <chrono>
#include <string>

#include "session/remote_process_session.h"

class SessionHealthPolicy {
public:
    // 尝试恢复主窗口：
    // 1) 先按 capture_pid 找主窗；
    // 2) 启动窗口期内，仅在 launch_pid 仍存活时，按 exe 名重绑，且窗口必须属于 launch_pid 或其子进程，
    //    避免误绑其它同名实例（例如已开着一个 notepad 时再启动一个）。
    static HWND try_recover_main_window(RemoteProcessSession& remote_process_session,
                                        DWORD launch_pid,
                                        DWORD& io_capture_pid,
                                        const std::string& target_exe_base_name,
                                        bool allow_pid_rebind_by_exename,
                                        bool had_successful_video,
                                        uint64_t pid_rebind_deadline_unix_ms);

    // 无窗口时按节流输出诊断，避免日志刷屏。
    static void maybe_log_no_window(RemoteProcessSession& remote_process_session,
                                    DWORD capture_pid,
                                    const std::string& target_exe_base_name,
                                    std::chrono::steady_clock::time_point& io_last_no_window_diag);

    // 判断是否应上报“远端已退出”：
    // 仅在曾成功出视频后，且窗口缺失持续超过 grace 窗口时才返回 true。
    static bool should_notify_remote_exit(bool had_successful_video,
                                          uint64_t now_ms,
                                          uint64_t& io_window_missing_since_unix_ms,
                                          uint32_t window_missing_exit_grace_ms);
};

