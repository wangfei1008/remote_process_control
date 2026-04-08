#pragma once

#include <windows.h>

#include <string>

namespace window_selection_utils {

// 统一 ASCII 小写规则，避免各处重复实现并降低行为偏差。
std::string to_lower_ascii(std::string s);
// Photoshop.exe -> photoshop
std::string exe_stem_lower(const std::string& exe_base_name);
std::string get_window_text_lower(HWND hwnd);
std::string get_window_class_lower(HWND hwnd);
// 窗口主评分：用于主窗选择、按 exe 匹配选择、以及可采集性判断。
int score_window_for_capture(HWND hwnd, DWORD expected_pid, bool require_pid_match);

} // namespace window_selection_utils

