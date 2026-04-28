#pragma once

// ============================================================
// capture_target_resolver.h
// 采集目标解析器：决定捕获哪个 PID + HWND
//
// 核心设计原则：
//   1. resolve() 是 const 纯查询，不修改任何状态
//   2. rebind 结果在返回值里，由调用方（CaptureSession）决定是否接受
//   3. 三类依赖全部通过构造注入，便于测试替换
//
// 职责边界：
//   YES - 按 PID 找窗口 / exe名称找窗口 / PID rebind 建议
//   NO  - 不直接调用 proc.set_capture_pid()（副作用交给调用方）
//   NO  - 不持有任何跨调用状态
// ============================================================

#include "../infra/win32_window.h"
#include "../infra/win32_process.h"
#include "window_score_policy.h"

#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace capture {

// ----------------------------------------------------------
// CaptureTargetInput：resolver 的只读输入
// ----------------------------------------------------------
struct CaptureTargetInput {
    DWORD            current_capture_pid = 0;
    DWORD            launch_pid          = 0;
    std::string_view target_basename;       // 小写，如 "notepad.exe"
    HWND             current_main_hwnd  = nullptr;
    // 调用方决定是否允许 rebind，而不是在 resolver 内部猜
    bool             allow_pid_rebind   = true;

    // Session Identity: stable PID set (e.g. Job object membership).
    std::span<const DWORD> session_pids{};
    bool             use_session_pids   = true;
};

// ----------------------------------------------------------
// CaptureTargetResult：resolver 的输出（纯数据，无副作用）
// ----------------------------------------------------------
struct CaptureTargetResult {
    DWORD                          capture_pid    = 0;
    HWND                           main_hwnd      = nullptr;
    DWORD                          main_hwnd_owner_pid = 0;
    std::vector<win32::WindowInfo> surfaces;

    // 诊断信息：单独字段，不污染主流程判断
    struct Diagnosis {
        DWORD       previous_capture_pid  = 0;
        bool        pid_rebound           = false;
        bool        used_exe_rebind       = false;
        bool        selected_from_surfaces = false;
        const char* reason                = "unknown";
    } diag;
};

// ----------------------------------------------------------
// CaptureTargetResolver
// ----------------------------------------------------------
class CaptureTargetResolver {
public:
    struct Deps {
        const win32::Window&       wops;
        const win32::Process&      prims;
        const WindowScorePolicy&   score_policy;
    };

    explicit CaptureTargetResolver(Deps deps);

    // 主接口：const，无副作用
    CaptureTargetResult resolve(const CaptureTargetInput& input) const;

private:
    // 在所有顶层窗口中按评分找最佳 HWND
    // accept_fn  : 返回 false 则跳过该窗口
    // bonus_fn   : 在基础分上叠加额外分（可为 nullptr）
    using AcceptFn = std::function<bool(const win32::WindowInfo&)>;
    using BonusFn  = std::function<int(const win32::WindowInfo&)>;

    HWND find_best_window(DWORD expected_pid, bool require_pid_match, const AcceptFn& accept, const BonusFn&  bonus) const;

    // 快路径：按 PID 精确找主窗口
    HWND find_main_window_by_pid(DWORD pid) const;

    // 恢复路径：在 launch_pid 进程树内按 exe basename 找窗口
    // 返回找到的 HWND，out_new_pid 输出窗口真实 owner PID
    HWND try_recover_by_exe(DWORD launch_pid, std::string_view basename, DWORD& out_new_pid) const;

    // 从 surfaces 列表中选面积最大的作为主窗口
    static HWND primary_from_surfaces(const std::vector<win32::WindowInfo>& surfaces);

    // 已知主窗口是否仍然可用
    bool is_main_hwnd_viable(HWND hwnd) const;

    Deps m_deps;
};

} // namespace capture
