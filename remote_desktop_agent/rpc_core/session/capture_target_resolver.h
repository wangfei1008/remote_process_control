#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

#include "common/window_ops.h"

class process_ops;

// Resolve which PID/HWND to capture for a launched process.
// This is a domain service: combines window facts (window_ops) + process facts (process_ops)
// with SessionHealthPolicy's recovery rules to produce a stable capture target.
struct CaptureTargetResolveResult {
    DWORD capture_pid = 0;
    DWORD previous_capture_pid = 0;
    DWORD main_hwnd_owner_pid = 0;
    HWND main_hwnd = nullptr;
    std::vector<window_ops::window_info> surfaces;
    const char* why = "";
    bool capture_pid_rebound = false;
    bool used_exe_rebind = false;
    bool main_hwnd_selected_from_surfaces = false;
};

class CaptureTargetResolver {
public:
    static CaptureTargetResolveResult resolve(process_ops& proc, DWORD launch_pid, DWORD& io_capture_pid, HWND current_main_hwnd, const std::string& target_exe_base_name_lower, bool had_successful_video, uint64_t pid_rebind_deadline_unix_ms);
};

