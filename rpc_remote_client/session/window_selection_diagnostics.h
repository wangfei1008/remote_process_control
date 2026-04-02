#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace window_selection_diagnostics {

struct WindowCandidateInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    int score = -1000000;
    int area = 0;
    std::string title;
    std::string cls;
    std::string base;
    bool stem_hit = false;
};

// 采集“与当前重绑定决策相关”的候选窗口（按 PID / exe basename / stem 命中过滤）。
void collect_rebind_candidates(
    DWORD capture_pid,
    const std::string& target_name,
    const std::string& stem,
    const std::function<std::string(DWORD)>& get_process_basename,
    std::vector<WindowCandidateInfo>& out_candidates);

void sort_candidates_by_rank(std::vector<WindowCandidateInfo>& candidates);

// 统一输出重绑定诊断摘要，避免业务流程文件里掺杂大量日志拼接细节。
void log_rebind_summary(
    DWORD capture_pid,
    const std::string& target_name,
    HWND by_pid,
    HWND by_exe,
    HWND by_hint,
    const std::vector<WindowCandidateInfo>& candidates);

} // namespace window_selection_diagnostics

