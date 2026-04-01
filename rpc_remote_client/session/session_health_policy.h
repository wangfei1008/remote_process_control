#pragma once

#include <windows.h>

#include <chrono>
#include <string>

#include "session/remote_process_session.h"

class SessionHealthPolicy {
public:
    static HWND try_recover_main_window(RemoteProcessSession& remote_process_session,
                                        DWORD& io_capture_pid,
                                        const std::string& target_exe_base_name,
                                        bool allow_pid_rebind_by_exename,
                                        bool had_successful_video,
                                        uint64_t pid_rebind_deadline_unix_ms);

    static void maybe_log_no_window(RemoteProcessSession& remote_process_session,
                                    DWORD capture_pid,
                                    std::chrono::steady_clock::time_point& io_last_no_window_diag);

    static bool should_notify_remote_exit(bool had_successful_video,
                                          uint64_t now_ms,
                                          uint64_t& io_window_missing_since_unix_ms,
                                          uint32_t window_missing_exit_grace_ms);
};

