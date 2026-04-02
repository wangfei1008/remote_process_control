#include "session/window_selection_diagnostics.h"

#include "session/window_selection_utils.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace window_selection_diagnostics {

void collect_rebind_candidates(
    DWORD capture_pid,
    const std::string& target_name,
    const std::string& stem,
    const std::function<std::string(DWORD)>& get_process_basename,
    std::vector<WindowCandidateInfo>& out_candidates)
{
    struct Enum_ctx {
        DWORD capture_pid = 0;
        const std::string* target = nullptr;
        const std::string* stem = nullptr;
        const std::function<std::string(DWORD)>* get_process_basename = nullptr;
        std::vector<WindowCandidateInfo>* out = nullptr;
    } enum_ctx{capture_pid, &target_name, &stem, &get_process_basename, &out_candidates};

    EnumWindows([](HWND hwnd, LPARAM l_param) -> BOOL {
        auto* ctx = reinterpret_cast<Enum_ctx*>(l_param);
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect)) return TRUE;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return TRUE;

        const std::string base = (*(ctx->get_process_basename))(pid);
        const std::string title = window_selection_utils::get_window_text_lower(hwnd);
        const std::string cls = window_selection_utils::get_window_class_lower(hwnd);
        const bool stem_hit = (!ctx->stem->empty() &&
                               (title.find(*(ctx->stem)) != std::string::npos ||
                                cls.find(*(ctx->stem)) != std::string::npos));
        const bool pid_hit = (pid == ctx->capture_pid);
        const bool base_hit = (!ctx->target->empty() && base == *(ctx->target));
        if (!pid_hit && !base_hit && !stem_hit) return TRUE;

        WindowCandidateInfo candidate;
        candidate.hwnd = hwnd;
        candidate.pid = pid;
        candidate.score = window_selection_utils::score_window_for_capture(hwnd, 0, false);
        candidate.area = width * height;
        candidate.title = title;
        candidate.cls = cls;
        candidate.base = base;
        candidate.stem_hit = stem_hit;
        ctx->out->push_back(std::move(candidate));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&enum_ctx));
}

void sort_candidates_by_rank(std::vector<WindowCandidateInfo>& candidates)
{
    std::sort(candidates.begin(), candidates.end(),
              [](const WindowCandidateInfo& a, const WindowCandidateInfo& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.area > b.area;
              });
}

void log_rebind_summary(
    DWORD capture_pid,
    const std::string& target_name,
    HWND by_pid,
    HWND by_exe,
    HWND by_hint,
    const std::vector<WindowCandidateInfo>& candidates)
{
    std::cout << "[proc][diag] recover capture_pid=" << capture_pid
              << " target_exe=" << target_name
              << " by_pid=" << reinterpret_cast<void*>(by_pid)
              << " by_exe=" << reinterpret_cast<void*>(by_exe)
              << " by_hint=" << reinterpret_cast<void*>(by_hint)
              << std::endl;

    if (candidates.empty()) {
        std::cout << "[proc][diag] candidates=0 for capture_pid/exe/hint filters" << std::endl;
        return;
    }

    std::cout << "[proc][diag] candidates=" << candidates.size() << " top=6" << std::endl;
    const size_t limit = (std::min)(static_cast<size_t>(6), candidates.size());
    for (size_t i = 0; i < limit; ++i) {
        const auto& c = candidates[i];
        std::ostringstream oss;
        oss << "[proc][diag] #" << i
            << " hwnd=" << reinterpret_cast<void*>(c.hwnd)
            << " pid=" << c.pid
            << " base=" << c.base
            << " score=" << c.score
            << " area=" << c.area
            << " stem_hit=" << (c.stem_hit ? 1 : 0)
            << " title=\"" << c.title << "\""
            << " class=\"" << c.cls << "\"";
        std::cout << oss.str() << std::endl;
    }
}

} // namespace window_selection_diagnostics

