#include "capture/capture_target_resolver.h"

#include "app/runtime_config.h"
#include "common/process_ops.h"
#include "common/character_conversion.h"
#include "common/rpc_time.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <sstream>

namespace {

struct BestWindow {
    HWND hwnd = nullptr;
    int score = -1000000;
    int area = 0;
};

static std::string get_window_text_lower(HWND hwnd)
{
    window_ops wops;
    return to_lower_ascii(wops.get_window_text_utf8(hwnd));
}

static std::string get_window_class_lower(HWND hwnd)
{
    window_ops wops;
    return to_lower_ascii(wops.get_window_class_utf8(hwnd));
}

static std::string exe_stem_lower(const std::string& exe_base_name)
{
    std::string stem = to_lower_ascii(exe_base_name);
    const auto pos = stem.rfind(".exe");
    if (pos != std::string::npos) {
        stem = stem.substr(0, pos);
    }
    return stem;
}

static bool contains_any_token(const std::string& s, const char* const* tokens, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (s.find(tokens[i]) != std::string::npos) return true;
    }
    return false;
}

static bool is_likely_splash_window(HWND hwnd)
{
    const std::string title = get_window_text_lower(hwnd);
    const std::string cls = get_window_class_lower(hwnd);
    static const char* kSplashTokens[] = { "splash", "loading", "startup", "welcome", "logo", "screen", "启动", "欢迎" };
    return contains_any_token(title, kSplashTokens, sizeof(kSplashTokens) / sizeof(kSplashTokens[0])) ||
           contains_any_token(cls, kSplashTokens, sizeof(kSplashTokens) / sizeof(kSplashTokens[0]));
}

static int score_window_for_capture(HWND hwnd, DWORD expected_pid, bool require_pid_match)
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
    if (area < 40000) score -= 80;
    else if (area < 120000) score -= 40;
    if (area < 120000 && !thickframe && !minimize_box && !maximize_box) score -= 30;
    score += (std::min)(30, area / 50000);
    return score;
}

static HWND find_best_window(DWORD expected_pid,
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
        if (bonus_score) score += bonus_score(hwnd, pid);

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

static HWND find_main_window_by_pid(DWORD pid)
{
    return find_best_window(pid, true, std::function<bool(HWND, DWORD)>(), std::function<int(HWND, DWORD)>());
}

static HWND primary_hwnd_from_surfaces(const std::vector<window_ops::window_info>& surfaces)
{
    if (surfaces.empty()) return nullptr;
    HWND best = nullptr;
    int best_area = -1;
    for (const auto& s : surfaces) {
        const int w = s.rect_screen.right - s.rect_screen.left;
        const int h = s.rect_screen.bottom - s.rect_screen.top;
        const int a = w * h;
        if (a > best_area) {
            best_area = a;
            best = s.hwnd;
        }
    }
    return best;
}

static HWND find_window_by_exe_basename(const process_ops& proc,
                                        const std::string& exe_base_name,
                                        DWORD anchor_process_root)
{
    if (exe_base_name.empty()) return nullptr;
    const std::string target_name = to_lower_ascii(exe_base_name);
    return find_best_window(
        0,
        false,
        [&proc, &target_name, anchor_process_root](HWND, DWORD pid) {
            if (anchor_process_root != 0 && !proc.is_same_or_descendant(pid, anchor_process_root)) return false;
            const std::string base_name = proc.get_process_basename_lower(pid);
            return !base_name.empty() && base_name == target_name;
        },
        std::function<int(HWND, DWORD)>());
}

static HWND find_window_by_exe_hint(const process_ops& proc,
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
            if (anchor_process_root != 0 && !proc.is_same_or_descendant(pid, anchor_process_root)) return false;
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

static HWND try_recover_main_window(const process_ops& proc, DWORD& io_capture_pid)
{
    // 目标：在“当前 capture_pid 找不到主窗口/窗口失效”的情况下，尽量恢复一个可用的主窗口 HWND。
    //
    // 设计要点（为什么需要这个恢复逻辑）：
    // - 某些进程启动后会发生 PID 漂移/重绑（例如：启动器进程拉起真正 UI 子进程；或应用重启渲染进程）。
    // - 在会话刚启动的短时间窗口内，如果我们仍按旧 pid 枚举窗口，可能一直找不到 surfaces。
    // - 因此允许在“尚未出过一次成功视频帧”时，基于 exe 名称在 launch_pid 的进程树内寻找更可信的 UI 窗口，
    //   并把 io_capture_pid 重绑到该窗口真实 owner PID，从而恢复采集目标。

    // 1) 快路径：只要按 io_capture_pid 能找到主窗口，就不做任何更激进的重绑。
    HWND main_window = find_main_window_by_pid(io_capture_pid);
	std::string target_exe_base_name = proc.target_exe_base_name_lower();

	DWORD launch_pid = proc.launch_pid();
    if (!main_window && !target_exe_base_name.empty() && launch_pid != 0 && proc.is_running(launch_pid)) {
        // 2) 恢复路径（严格受限）：
        // - 只在尚未成功出画前启用：避免稳定运行后因为偶发窗口枚举抖动而把 pid 错绑到别的进程。
        // - 只在 pid_rebind_deadline_unix_ms 前启用：把“允许重绑”的时窗限制在启动早期。
        // - 只在 launch_pid 仍存活时启用：以 launch_pid 作为“进程树锚点”，避免误命中系统上同名窗口。

        // 2.1 优先按 exe basename 精确匹配（例如 notepad.exe）。
        HWND hwnd = find_window_by_exe_basename(proc, target_exe_base_name, launch_pid);
        bool by_hint = false;
        if (!hwnd) {
            // 2.2 次选：按 exe stem 在 window title/class 中做 hint 匹配（容错，风险也更高）。
            hwnd = find_window_by_exe_hint(proc, target_exe_base_name, launch_pid);
            by_hint = (hwnd != nullptr);
        }
        if (hwnd) {
            window_ops wops;
            const DWORD real_pid = wops.get_window_pid(hwnd);
            if (real_pid) {
                // 2.3 一旦找到候选窗口，就以窗口真实 owner PID 作为新的 capture_pid（这一步才真正“重绑”）。
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

static bool is_window_viable_for_capture(HWND hwnd)
{
    window_ops wops;
    if (!wops.is_valid(hwnd)) return false;
    const int score = score_window_for_capture(hwnd, 0, false);
    return score > 0;
}

} // namespace

CaptureTargetResolveResult CaptureTargetResolver::resolve(process_ops& proc, HWND current_main_hwnd)
{
	DWORD io_capture_pid = proc.capture_pid();
    CaptureTargetResolveResult r;
    r.previous_capture_pid = io_capture_pid;
    r.capture_pid = io_capture_pid;
    r.main_hwnd = current_main_hwnd;

    window_ops wops;

    // 1) If we already have a viable main window, prefer its owner PID for capture.
    if (r.main_hwnd && is_window_viable_for_capture(r.main_hwnd)) {
        const DWORD owner_pid = wops.get_window_pid(r.main_hwnd);
        r.main_hwnd_owner_pid = owner_pid;
        if (owner_pid != 0 && owner_pid != io_capture_pid) {
            io_capture_pid = owner_pid;
			proc.set_capture_pid(io_capture_pid);
            r.capture_pid = owner_pid;
            r.capture_pid_rebound = true;
            r.why = "main_hwnd_owner_pid";
        } else {
            r.why = "main_hwnd_ok";
        }
        r.surfaces = wops.enumerate_visible_top_level(r.capture_pid);
        if (!r.surfaces.empty()) {
            // Ensure main hwnd stays in sync with surfaces (may change when UI rearranges).
            r.main_hwnd = primary_hwnd_from_surfaces(r.surfaces);
            r.main_hwnd_selected_from_surfaces = true;
            r.main_hwnd_owner_pid = wops.get_window_pid(r.main_hwnd);
        }
        return r;
    }

    // 2) Enumerate by current capture PID first (fast path).
    if (io_capture_pid != 0) {
        r.surfaces = wops.enumerate_visible_top_level(io_capture_pid);
        if (!r.surfaces.empty()) {
            r.capture_pid = io_capture_pid;
            r.main_hwnd = primary_hwnd_from_surfaces(r.surfaces);
            r.main_hwnd_selected_from_surfaces = true;
            r.main_hwnd_owner_pid = wops.get_window_pid(r.main_hwnd);
            r.why = "surfaces_by_capture_pid";
            return r;
        }
    }

    // 3) Recovery path: let policy attempt to find/rebind main window (by PID then exe-name within startup window).
    DWORD cap = io_capture_pid;
    HWND recovered = try_recover_main_window(proc, cap);

    if (cap != io_capture_pid) {
        io_capture_pid = cap;
        proc.set_capture_pid(io_capture_pid);
        r.capture_pid_rebound = true;
        r.used_exe_rebind = true;
    }
    r.capture_pid = io_capture_pid;
    r.main_hwnd = recovered;

    if (recovered) {
        const DWORD owner_pid = wops.get_window_pid(recovered);
        r.main_hwnd_owner_pid = owner_pid;
        if (owner_pid != 0 && owner_pid != io_capture_pid) {
            // Use the window's real owner PID for enumeration.
            io_capture_pid = owner_pid;
            proc.set_capture_pid(io_capture_pid);
            r.capture_pid = owner_pid;
            r.capture_pid_rebound = true;
            r.why = "recovered_hwnd_owner_pid";
        } else {
            r.why = "recovered_hwnd";
        }
    } else {
        r.why = "recover_failed";
    }

    if (r.capture_pid != 0) {
        r.surfaces = wops.enumerate_visible_top_level(r.capture_pid);
    }
    if (!r.surfaces.empty()) {
        // Prefer picking from surfaces so capture & input mapping stays consistent.
        r.main_hwnd = primary_hwnd_from_surfaces(r.surfaces);
        r.main_hwnd_selected_from_surfaces = true;
        r.main_hwnd_owner_pid = wops.get_window_pid(r.main_hwnd);
    }
    return r;
}

