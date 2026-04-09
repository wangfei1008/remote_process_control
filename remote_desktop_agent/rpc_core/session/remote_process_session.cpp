#include "session/remote_process_session.h"
#include "session/process_lifecycle.h"
#include "session/window_selection_diagnostics.h"
#include "session/window_selection_utils.h"

#include <functional>
#include <iostream>

namespace {

struct BestWindow {
    HWND hwnd = nullptr;
    int score = -1000000;
    int area = 0;
};

// 通用窗口择优器：
// 1) 统一遍历窗口与基础评分；
// 2) 由调用方提供过滤条件与附加分，避免多处复制“枚举 + 评分 + 取最大”模板代码。
static HWND find_best_window(
    DWORD expected_pid,
    bool require_pid_match,
    const std::function<bool(HWND, DWORD)>& accept,
    const std::function<int(HWND, DWORD)>& bonus_score)
{
    struct EnumCtx {
        DWORD expected_pid = 0;
        bool require_pid_match = false;
        const std::function<bool(HWND, DWORD)>* accept = nullptr;
        const std::function<int(HWND, DWORD)>* bonus_score = nullptr;
        BestWindow* best = nullptr;
    } ctx{expected_pid, require_pid_match, &accept, &bonus_score, nullptr};

    BestWindow best;
    ctx.best = &best;

    EnumWindows([](HWND hwnd, LPARAM l_param) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(l_param);

        RECT rect{};
        if (!GetWindowRect(hwnd, &rect)) return TRUE;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return TRUE;

        // std::function 可能为空：指针非空不代表可调用，需先判断 *func 是否有目标。
        if (c->accept && *c->accept && !(*(c->accept))(hwnd, pid)) return TRUE;

        int score = window_selection_utils::score_window_for_capture(
            hwnd, c->expected_pid, c->require_pid_match);
        if (score <= -1000000) return TRUE;
        if (c->bonus_score && *c->bonus_score) score += (*(c->bonus_score))(hwnd, pid);

        const int area = width * height;
        if (score > c->best->score || (score == c->best->score && area > c->best->area)) {
            c->best->score = score;
            c->best->area = area;
            c->best->hwnd = hwnd;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return best.hwnd;
}

} // namespace

bool RemoteProcessSession::launch_process(const std::string& exe_path,
                                          PROCESS_INFORMATION& out_process_info,
                                          DWORD& out_launch_pid,
                                          DWORD& out_capture_pid,
                                          HWND& out_main_window,
                                          std::string& out_target_exe_base_name)
{
    if (!process_lifecycle::launch_process(
            exe_path, out_process_info, out_launch_pid, out_capture_pid, out_target_exe_base_name)) {
        return false;
    }

    // 不要因等待窗口而阻塞流创建。
    out_main_window = find_main_window(out_process_info.dwProcessId);
    return true;
}

void RemoteProcessSession::terminate_processes(PROCESS_INFORMATION& process_info, DWORD capture_pid, DWORD launch_pid)
{
    process_lifecycle::terminate_processes(process_info, capture_pid, launch_pid);
}

std::vector<HWND> RemoteProcessSession::find_all_windows(DWORD pid) const
{
    struct Enum_data {
        DWORD pid = 0;
        std::vector<HWND>* hwnds = nullptr;
    } enum_data{pid, nullptr};

    std::vector<HWND> result;
    enum_data.hwnds = &result;

    EnumWindows(static_cast<WNDENUMPROC>([](HWND hwnd, LPARAM l_param) -> BOOL {
        auto* data = reinterpret_cast<Enum_data*>(l_param);
        DWORD window_pid = 0;
        GetWindowThreadProcessId(hwnd, &window_pid);
        if (window_pid == data->pid) {
            data->hwnds->push_back(hwnd);
        }
        return TRUE;
    }), reinterpret_cast<LPARAM>(&enum_data));

    return result;
}

HWND RemoteProcessSession::find_main_window(DWORD pid) const
{
    return find_best_window(
        pid,
        true,
        std::function<bool(HWND, DWORD)>(),
        std::function<int(HWND, DWORD)>());
}

HWND RemoteProcessSession::find_window_by_exe_basename(const std::string& exe_base_name,
                                                          DWORD anchor_process_root) const
{
    if (exe_base_name.empty()) return nullptr;
    const std::string target_name = window_selection_utils::to_lower_ascii(exe_base_name);
    return find_best_window(
        0,
        false,
        [&target_name, anchor_process_root](HWND, DWORD pid) {
            if (anchor_process_root != 0 &&
                !process_lifecycle::pid_is_same_or_descendant(pid, anchor_process_root)) {
                return false;
            }
            const std::string base_name = process_lifecycle::get_process_basename(pid);
            return !base_name.empty() && base_name == target_name;
        },
        std::function<int(HWND, DWORD)>());
}

HWND RemoteProcessSession::find_window_by_exe_hint(const std::string& exe_base_name,
                                                     DWORD anchor_process_root) const
{
    if (exe_base_name.empty()) return nullptr;
    const std::string stem = window_selection_utils::exe_stem_lower(exe_base_name);
    if (stem.empty()) return nullptr;
    return find_best_window(
        0,
        false,
        [&stem, anchor_process_root](HWND hwnd, DWORD pid) {
            if (anchor_process_root != 0 &&
                !process_lifecycle::pid_is_same_or_descendant(pid, anchor_process_root)) {
                return false;
            }
            if (!IsWindowVisible(hwnd)) return false;
            if (GetWindow(hwnd, GW_OWNER) != NULL) return false;
            const std::string title = window_selection_utils::get_window_text_lower(hwnd);
            const std::string cls = window_selection_utils::get_window_class_lower(hwnd);
            return title.find(stem) != std::string::npos || cls.find(stem) != std::string::npos;
        },
        [&stem](HWND hwnd, DWORD) {
            // hint 路径下，标题/类名命中 exe stem 作为附加分，而不是替代基础评分。
            int bonus = 0;
            const std::string title = window_selection_utils::get_window_text_lower(hwnd);
            const std::string cls = window_selection_utils::get_window_class_lower(hwnd);
            if (title.find(stem) != std::string::npos) bonus += 12;
            if (cls.find(stem) != std::string::npos) bonus += 6;
            return bonus;
        });
}

bool RemoteProcessSession::is_window_viable_for_capture(HWND hwnd) const
{
    if (!hwnd || !IsWindow(hwnd)) return false;
    const int score = window_selection_utils::score_window_for_capture(hwnd, 0, false);
    return score > 0;
}

void RemoteProcessSession::log_window_candidates_for_rebind(DWORD capture_pid, const std::string& target_exe_base_name) const
{
    // 该函数只负责“诊断流程编排”；候选采集/排序/输出细节下沉到 diagnostics 模块。
    const std::string target_name = window_selection_utils::to_lower_ascii(target_exe_base_name);
    const std::string stem = window_selection_utils::exe_stem_lower(target_exe_base_name);
    const HWND by_pid = find_main_window(capture_pid);
    const HWND by_exe = target_name.empty() ? nullptr : find_window_by_exe_basename(target_name);
    const HWND by_hint = target_name.empty() ? nullptr : find_window_by_exe_hint(target_name);

    std::vector<window_selection_diagnostics::WindowCandidateInfo> candidates;
    window_selection_diagnostics::collect_rebind_candidates(
        capture_pid,
        target_name,
        stem,
        [](DWORD pid) { return process_lifecycle::get_process_basename(pid); },
        candidates);
    window_selection_diagnostics::sort_candidates_by_rank(candidates);
    window_selection_diagnostics::log_rebind_summary(capture_pid, target_name, by_pid, by_exe, by_hint, candidates);
}

