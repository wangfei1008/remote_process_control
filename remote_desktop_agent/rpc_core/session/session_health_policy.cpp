#include "session/session_health_policy.h"

#include "common/process_ops.h"
#include "common/character_conversion.h"
#include "common/rpc_time.h"
#include "common/window_ops.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <sstream>
#include <vector>

namespace {

struct BestWindow {
    HWND hwnd = nullptr;
    int score = -1000000;
    int area = 0;
};

// --- 原 window_selection_utils：字符串与窗口评分 ---

std::string get_window_text_lower(HWND hwnd)
{
    window_ops wops;
    return to_lower_ascii(wops.get_window_text_utf8(hwnd));
}

std::string get_window_class_lower(HWND hwnd)
{
    window_ops wops;
    return to_lower_ascii(wops.get_window_class_utf8(hwnd));
}

std::string exe_stem_lower(const std::string& exe_base_name)
{
    std::string stem = to_lower_ascii(exe_base_name);
    const auto pos = stem.rfind(".exe");
    if (pos != std::string::npos) {
        stem = stem.substr(0, pos);
    }
    return stem;
}

bool contains_any_token(const std::string& s, const char* const* tokens, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (s.find(tokens[i]) != std::string::npos) return true;
    }
    return false;
}

bool is_likely_splash_window(HWND hwnd)
{
    const std::string title = get_window_text_lower(hwnd);
    const std::string cls = get_window_class_lower(hwnd);
    static const char* kSplashTokens[] = {
        "splash", "loading", "startup", "welcome", "logo", "screen",
        "启动", "欢迎"
    };
    return contains_any_token(title, kSplashTokens, sizeof(kSplashTokens) / sizeof(kSplashTokens[0])) ||
           contains_any_token(cls, kSplashTokens, sizeof(kSplashTokens) / sizeof(kSplashTokens[0]));
}

int score_window_for_capture(HWND hwnd, DWORD expected_pid, bool require_pid_match)
{
    window_ops wops;
    if (!wops.is_valid(hwnd)) return -1000000;
    const DWORD pid = wops.get_window_pid(hwnd);
    if (!pid) return -1000000;
    if (require_pid_match && pid != expected_pid) return -1000000;

    RECT rc{};
    if (!wops.get_window_rect(hwnd, rc)) return -1000000;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return -1000000;

    const LONG_PTR style = wops.get_style(hwnd);
    const LONG_PTR ex_style = wops.get_ex_style(hwnd);
    const bool visible = wops.is_visible(hwnd);
    const bool ownerless = wops.is_ownerless_top_level(hwnd);
    const bool tool = ((ex_style & WS_EX_TOOLWINDOW) != 0);
    const bool app_window = ((ex_style & WS_EX_APPWINDOW) != 0);
    const bool caption = ((style & WS_CAPTION) != 0);
    const bool thickframe = ((style & WS_THICKFRAME) != 0);
    const bool minimize_box = ((style & WS_MINIMIZEBOX) != 0);
    const bool maximize_box = ((style & WS_MAXIMIZEBOX) != 0);
    const bool popup_only = ((style & WS_POPUP) != 0) && !caption;

    int score = 0;
    if (visible) score += 40;
    if (ownerless) score += 20;
    if (!tool) score += 20;
    if (app_window) score += 12;
    if (caption) score += 10;
    if (thickframe) score += 8;
    if (minimize_box) score += 6;
    if (maximize_box) score += 6;
    if (popup_only) score -= 16;
    if (tool) score -= 20;
    if (is_likely_splash_window(hwnd)) score -= 80;

    const int area = width * height;
    if (area < 40000) {
        score -= 80;
    } else if (area < 120000) {
        score -= 40;
    }
    if (area < 120000 && !thickframe && !minimize_box && !maximize_box) {
        score -= 30;
    }
    score += (std::min)(30, area / 50000);
    return score;
}

// --- 原 window_selection_diagnostics ---

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

void collect_rebind_candidates(
    DWORD capture_pid,
    const std::string& target_name,
    const std::string& stem,
    const std::function<std::string(DWORD)>& get_process_basename,
    std::vector<WindowCandidateInfo>& out_candidates)
{
    window_ops wops;
    wops.enumerate_top_level_windows([&](HWND hwnd) -> bool {
        RECT rect{};
        if (!wops.get_window_rect(hwnd, rect)) return true;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return true;

        const DWORD pid = wops.get_window_pid(hwnd);
        if (!pid) return true;

        const std::string base = get_process_basename(pid);
        const std::string title = get_window_text_lower(hwnd);
        const std::string cls = get_window_class_lower(hwnd);
        const bool stem_hit = (!stem.empty() &&
                               (title.find(stem) != std::string::npos ||
                                cls.find(stem) != std::string::npos));
        const bool pid_hit = (pid == capture_pid);
        const bool base_hit = (!target_name.empty() && base == target_name);
        if (!pid_hit && !base_hit && !stem_hit) return true;

        WindowCandidateInfo candidate;
        candidate.hwnd = hwnd;
        candidate.pid = pid;
        candidate.score = score_window_for_capture(hwnd, 0, false);
        candidate.area = width * height;
        candidate.title = title;
        candidate.cls = cls;
        candidate.base = base;
        candidate.stem_hit = stem_hit;
        out_candidates.push_back(std::move(candidate));
        return true;
    });
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

// --- 窗口择优 ---

HWND find_best_window(
    DWORD expected_pid,
    bool require_pid_match,
    const std::function<bool(HWND, DWORD)>& accept,
    const std::function<int(HWND, DWORD)>& bonus_score)
{
    window_ops wops;
    BestWindow best;

    wops.enumerate_top_level_windows([&](HWND hwnd) -> bool {
        RECT rect{};
        if (!wops.get_window_rect(hwnd, rect)) return true;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return true;

        const DWORD pid = wops.get_window_pid(hwnd);
        if (!pid) return true;

        if (accept && !accept(hwnd, pid)) return true;

        int score = score_window_for_capture(hwnd, expected_pid, require_pid_match);
        if (score <= -1000000) return true;
        if (bonus_score) {
            score += bonus_score(hwnd, pid);
        }

        const int area = width * height;
        if (score > best.score || (score == best.score && area > best.area)) {
            best.score = score;
            best.area = area;
            best.hwnd = hwnd;
        }
        return true;
    });

    return best.hwnd;
}

} // namespace

std::vector<HWND> SessionHealthPolicy::find_all_windows(DWORD pid)
{
    std::vector<HWND> result;
    window_ops wops;
    wops.enumerate_top_level_windows([&](HWND hwnd) -> bool {
        if (wops.get_window_pid(hwnd) == pid) {
            result.push_back(hwnd);
        }
        return true;
    });
    return result;
}

HWND SessionHealthPolicy::find_main_window(DWORD pid)
{
    return find_best_window(pid, true, std::function<bool(HWND, DWORD)>(),  std::function<int(HWND, DWORD)>());
}

HWND SessionHealthPolicy::find_window_by_exe_basename(const process_ops& proc,
                                                      const std::string& exe_base_name,
                                                      DWORD anchor_process_root)
{
    if (exe_base_name.empty()) return nullptr;
    const std::string target_name = to_lower_ascii(exe_base_name);
    return find_best_window(
        0,
        false,
        [&proc, &target_name, anchor_process_root](HWND, DWORD pid) {
            if (anchor_process_root != 0 &&
                !proc.is_same_or_descendant(pid, anchor_process_root)) {
                return false;
            }
            const std::string base_name = proc.get_process_basename_lower(pid);
            return !base_name.empty() && base_name == target_name;
        },
        std::function<int(HWND, DWORD)>());
}

HWND SessionHealthPolicy::find_window_by_exe_hint(const process_ops& proc,
                                                    const std::string& exe_base_name,
                                                    DWORD anchor_process_root)
{
    if (exe_base_name.empty()) return nullptr;
    const std::string stem = exe_stem_lower(exe_base_name);
    if (stem.empty()) return nullptr;
    window_ops wops;
    return find_best_window(
        0,
        false,
        [&proc, &stem, anchor_process_root, &wops](HWND hwnd, DWORD pid) {
            if (anchor_process_root != 0 &&
                !proc.is_same_or_descendant(pid, anchor_process_root)) {
                return false;
            }
            if (!wops.is_visible(hwnd)) return false;
            if (!wops.is_ownerless_top_level(hwnd)) return false;
            const std::string title = get_window_text_lower(hwnd);
            const std::string cls = get_window_class_lower(hwnd);
            return title.find(stem) != std::string::npos || cls.find(stem) != std::string::npos;
        },
        [&stem](HWND hwnd, DWORD) {
            int bonus = 0;
            const std::string title = get_window_text_lower(hwnd);
            const std::string cls = get_window_class_lower(hwnd);
            if (title.find(stem) != std::string::npos) bonus += 12;
            if (cls.find(stem) != std::string::npos) bonus += 6;
            return bonus;
        });
}

bool SessionHealthPolicy::is_window_viable_for_capture(HWND hwnd)
{
    window_ops wops;
    if (!wops.is_valid(hwnd)) return false;
    const int score = score_window_for_capture(hwnd, 0, false);
    return score > 0;
}

void SessionHealthPolicy::log_window_candidates_for_rebind(const process_ops& proc,
                                                           DWORD capture_pid,
                                                           const std::string& target_exe_base_name)
{
    const std::string target_name = to_lower_ascii(target_exe_base_name);
    const std::string stem = exe_stem_lower(target_exe_base_name);
    const HWND by_pid = find_main_window(capture_pid);
    const HWND by_exe = target_name.empty() ? nullptr : find_window_by_exe_basename(proc, target_name);
    const HWND by_hint = target_name.empty() ? nullptr : find_window_by_exe_hint(proc, target_name);

    std::vector<WindowCandidateInfo> candidates;
    collect_rebind_candidates(
        capture_pid,
        target_name,
        stem,
        [&proc](DWORD pid) { return proc.get_process_basename_lower(pid); },
        candidates);
    sort_candidates_by_rank(candidates);
    log_rebind_summary(capture_pid, target_name, by_pid, by_exe, by_hint, candidates);
}

HWND SessionHealthPolicy::try_recover_main_window(const process_ops& proc,
                                                  DWORD launch_pid,
                                                  DWORD& io_capture_pid,
                                                  const std::string& target_exe_base_name,
                                                  bool allow_pid_rebind_by_exename,
                                                  bool had_successful_video,
                                                  uint64_t pid_rebind_deadline_unix_ms)
{
    HWND main_window = find_main_window(io_capture_pid);

    if (allow_pid_rebind_by_exename &&
        !had_successful_video &&
        !main_window &&
        !target_exe_base_name.empty() &&
        rpc_unix_epoch_ms() <= pid_rebind_deadline_unix_ms &&
        launch_pid != 0 &&
        proc.is_running(launch_pid)) {
        HWND hwnd = find_window_by_exe_basename(proc, target_exe_base_name, launch_pid);
        bool by_hint = false;
        if (!hwnd) {
            hwnd = find_window_by_exe_hint(proc, target_exe_base_name, launch_pid);
            by_hint = (hwnd != nullptr);
        }
        if (hwnd) {
            window_ops wops;
            const DWORD real_pid = wops.get_window_pid(hwnd);
            if (real_pid) {
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


bool SessionHealthPolicy::should_notify_remote_exit(bool had_successful_video,
                                                    uint64_t now_ms,
                                                    uint64_t& io_window_missing_since_unix_ms,
                                                    uint32_t window_missing_exit_grace_ms)
{
    if (io_window_missing_since_unix_ms == 0)
        io_window_missing_since_unix_ms = now_ms;

    if (!had_successful_video || now_ms < io_window_missing_since_unix_ms) return false;

    return (now_ms - io_window_missing_since_unix_ms) >= static_cast<uint64_t>(window_missing_exit_grace_ms);
}
