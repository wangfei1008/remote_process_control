// ============================================================
// capture_target_resolver.cpp
// ============================================================

#include "capture_target_resolver.h"
#include "common/rpc_time.h"
#include <algorithm>
#include <limits>
#include <iostream>
#include <functional>
#include <unordered_set>

namespace capture {

// ---- 构造 ---------------------------------------------------

CaptureTargetResolver::CaptureTargetResolver(Deps deps)
    : m_deps(deps) {}

// ---- 内部工具 -----------------------------------------------

HWND CaptureTargetResolver::find_best_window(DWORD expected_pid, bool require_pid_match, const AcceptFn& accept, const BonusFn&  bonus) const {
    HWND best_hwnd  = nullptr;
    // Windows headers may define min/max macros; use the "callable min" form.
    int  best_score = (std::numeric_limits<int>::min)();
    int  best_area  = 0;

    const WindowScorePolicy::Context ctx{ expected_pid, require_pid_match };

    m_deps.wops.enumerate_top_level([&](HWND hwnd) -> bool {
        win32::WindowInfo info = m_deps.wops.snapshot(hwnd);
        if (!info.valid) return true;

        const int w = info.rect_screen.right  - info.rect_screen.left;
        const int h = info.rect_screen.bottom - info.rect_screen.top;
        if (w <= 0 || h <= 0) return true;

        if (accept && !accept(info)) return true;

        auto opt = m_deps.score_policy.score(info, ctx);
        if (!opt.has_value()) return true;

        int s = *opt;
        if (bonus) s += bonus(info);

        const int area = w * h;
        if (s > best_score || (s == best_score && area > best_area)) {
            best_score = s;
            best_area  = area;
            best_hwnd  = hwnd;
        }
        return true;
    });

    return best_hwnd;
}

HWND CaptureTargetResolver::find_main_window_by_pid(DWORD pid) const {
    return find_best_window(pid, true, nullptr, nullptr);
}

HWND CaptureTargetResolver::try_recover_by_exe(DWORD launch_pid, std::string_view basename, DWORD& out_new_pid) const {
    out_new_pid = 0;
    if (basename.empty() || launch_pid == 0) return nullptr;

    const std::string target(basename);
    // 1) 按 exe basename 精确匹配进程
    HWND hwnd = find_best_window(
        0, false,
        [&](const win32::WindowInfo& info) -> bool {
            if (!m_deps.prims.is_descendant_of(info.pid, launch_pid)) return false;
            return m_deps.prims.basename_lower(info.pid) == target;
        },
        nullptr);

    bool by_hint = false;

    if (!hwnd) {
        // 2) 次选：在 title / class 中做 stem hint 匹配（去掉 .exe 后缀）
        std::string stem = target;
        const auto pos = stem.rfind(".exe");
        if (pos != std::string::npos) stem = stem.substr(0, pos);

        if (!stem.empty()) {
            hwnd = find_best_window(
                0, false,
                [&](const win32::WindowInfo& info) -> bool {
                    if (!m_deps.prims.is_descendant_of(info.pid, launch_pid)) return false;
                    if (!info.visible || !info.ownerless) return false;
                    return info.title.find(stem)      != std::string::npos
                        || info.class_name.find(stem) != std::string::npos;
                },
                [&](const win32::WindowInfo& info) -> int {
                    int bonus = 0;
                    if (info.title.find(stem)      != std::string::npos) bonus += 12;
                    if (info.class_name.find(stem) != std::string::npos) bonus +=  6;
                    return bonus;
                });
            by_hint = (hwnd != nullptr);
        }
    }

    if (!hwnd) return nullptr;

    const DWORD real_pid = m_deps.wops.pid(hwnd);
    if (real_pid == 0) return nullptr;

    out_new_pid = real_pid;

    if (by_hint) {
        std::cout << "[resolver] recovered by exe-hint"
                  << " pid=" << real_pid
                  << " exe=" << target << '\n';
    }
    std::cout << "[resolver] time 3.1.5 = " << rpc_unix_epoch_ms() << std::endl;
    return hwnd;
}

/*static*/
HWND CaptureTargetResolver::primary_from_surfaces(const std::vector<win32::WindowInfo>& surfaces) {

    if (surfaces.empty()) return nullptr;
    HWND best      = nullptr;
    int  best_area = -1;
    for (const auto& s : surfaces) {
        const int w = s.rect_screen.right  - s.rect_screen.left;
        const int h = s.rect_screen.bottom - s.rect_screen.top;
        if (w * h > best_area) { best_area = w * h; best = s.hwnd; }
    }
    return best;
}

namespace {

static bool pid_in_set(DWORD pid, const std::unordered_set<DWORD>& set)
{
    if (pid == 0) return false;
    return set.find(pid) != set.end();
}

static void append_surfaces_for_pid(const win32::Window& wops,
                                   DWORD pid,
                                   std::vector<win32::WindowInfo>& io_out)
{
    auto v = wops.enumerate_visible_top_level(pid);
    io_out.insert(io_out.end(), v.begin(), v.end());
}

static void dedup_surfaces_by_hwnd(std::vector<win32::WindowInfo>& io_surfaces)
{
    std::unordered_set<HWND> seen;
    std::vector<win32::WindowInfo> out;
    out.reserve(io_surfaces.size());
    for (auto& s : io_surfaces) {
        if (!s.hwnd) continue;
        if (seen.insert(s.hwnd).second) out.push_back(std::move(s));
    }
    io_surfaces.swap(out);
}

} // namespace

bool CaptureTargetResolver::is_main_hwnd_viable(HWND hwnd) const {
    if (!hwnd) return false;
    win32::WindowInfo info = m_deps.wops.snapshot(hwnd);
    auto opt = m_deps.score_policy.score(info, {});
    return opt.has_value() && *opt > 0;
}

// ---- 主接口 -------------------------------------------------

CaptureTargetResult CaptureTargetResolver::resolve( const CaptureTargetInput& input) const {
    CaptureTargetResult r;
    r.capture_pid              = input.current_capture_pid;
    r.main_hwnd                = input.current_main_hwnd;
    r.diag.previous_capture_pid = input.current_capture_pid;

    DWORD cap_pid = input.current_capture_pid;

    // ----------------------------------------------------------
    // 路径 0：Session Identity（Job PID 集合）优先
    // ----------------------------------------------------------
    if (input.use_session_pids && !input.session_pids.empty()) {
        std::unordered_set<DWORD> pidset;
        pidset.reserve(input.session_pids.size() * 2);
        for (DWORD p : input.session_pids) {
            if (p) pidset.insert(p);
        }

        // surfaces: union of all visible top-level windows for every pid in the session set
        r.surfaces.clear();
        r.surfaces.reserve(32);
        for (DWORD p : pidset) {
            append_surfaces_for_pid(m_deps.wops, p, r.surfaces);
        }
        dedup_surfaces_by_hwnd(r.surfaces);

        if (!r.surfaces.empty()) {
            r.main_hwnd = primary_from_surfaces(r.surfaces);
            r.main_hwnd_owner_pid = m_deps.wops.pid(r.main_hwnd);
            r.capture_pid = r.main_hwnd_owner_pid ? r.main_hwnd_owner_pid : input.current_capture_pid;
            if (r.capture_pid != input.current_capture_pid && r.capture_pid != 0) {
                r.diag.pid_rebound = true;
            }
            r.diag.selected_from_surfaces = true;
            r.diag.reason = "surfaces_by_session_pids";
            return r;
        }
    }

    // ----------------------------------------------------------
    // 路径 1：已有可用主窗口 → 直接用其 owner PID
    // ----------------------------------------------------------
    if (is_main_hwnd_viable(r.main_hwnd)) {
        const DWORD owner = m_deps.wops.pid(r.main_hwnd);
        r.main_hwnd_owner_pid = owner;

        if (owner != 0 && owner != cap_pid) {
            cap_pid              = owner;
            r.capture_pid        = owner;
            r.diag.pid_rebound   = true;
            r.diag.reason        = "main_hwnd_owner_pid";
        } else {
            r.diag.reason = "main_hwnd_ok";
        }

        r.surfaces = m_deps.wops.enumerate_visible_top_level(cap_pid);
        if (!r.surfaces.empty()) {
            r.main_hwnd                    = primary_from_surfaces(r.surfaces);
            r.main_hwnd_owner_pid          = m_deps.wops.pid(r.main_hwnd);
            r.diag.selected_from_surfaces  = true;
        }
        return r;
    }

    // ----------------------------------------------------------
    // 路径 2：按当前 capture_pid 枚举 surfaces（快路径）
    // ----------------------------------------------------------
    if (cap_pid != 0) {
        r.surfaces = m_deps.wops.enumerate_visible_top_level(cap_pid);
        if (!r.surfaces.empty()) {
            r.main_hwnd                   = primary_from_surfaces(r.surfaces);
            r.main_hwnd_owner_pid         = m_deps.wops.pid(r.main_hwnd);
            r.diag.selected_from_surfaces = true;
            r.diag.reason                 = "surfaces_by_capture_pid";
            return r;
        }
    }

    // ----------------------------------------------------------
    // 路径 3：恢复路径（仅当 allow_pid_rebind 且 launch_pid 存活）
    // ----------------------------------------------------------
    if (input.allow_pid_rebind
        && input.launch_pid != 0
        && !input.target_basename.empty()
        && m_deps.prims.is_running(input.launch_pid)) {

        DWORD new_pid = 0;
        HWND recovered = try_recover_by_exe(input.launch_pid, input.target_basename, new_pid);

        if (recovered && new_pid != 0) {
            if (new_pid != cap_pid) {
                std::cout << "[resolver] pid rebound"
                          << " old=" << cap_pid
                          << " new=" << new_pid
                          << " exe=" << input.target_basename << '\n';
                r.diag.pid_rebound     = true;
                r.diag.used_exe_rebind = true;
            }
            cap_pid               = new_pid;
            r.capture_pid         = new_pid;
            r.main_hwnd           = recovered;
            r.main_hwnd_owner_pid = new_pid;
            r.diag.reason         = "exe_rebind";
        } else {
            r.diag.reason = "recover_failed";
        }
    } else {
        r.diag.reason = "no_rebind_allowed";
    }

    // 用最终确定的 PID 刷新 surfaces
    if (r.capture_pid != 0) {
        r.surfaces = m_deps.wops.enumerate_visible_top_level(r.capture_pid);
    }
    if (!r.surfaces.empty()) {
        r.main_hwnd                   = primary_from_surfaces(r.surfaces);
        r.main_hwnd_owner_pid         = m_deps.wops.pid(r.main_hwnd);
        r.diag.selected_from_surfaces = true;
    }

    return r;
}

} // namespace capture