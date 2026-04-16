#pragma once

#include <windows.h>

#include <chrono>
#include <string>
#include <vector>

class process_ops;

// 会话健康与「采集目标窗口」发现策略（主窗 / exe 匹配 / hint / 诊断日志）。
// 实现上合并了原 window_selection_utils / window_selection_diagnostics（单编译单元，对外仅此类型）。
class SessionHealthPolicy {
public:
    // === Public API（业务/引擎会直接调用）===

    /// 按 PID 选最优可采集主窗（仅依赖窗口枚举与评分，不查进程镜像）。
    static HWND find_main_window(DWORD pid);


    static bool is_window_viable_for_capture(HWND hwnd);


    // 尝试恢复主窗口：
    // 1) 先按 capture_pid 找主窗；
    // 2) 启动窗口期内，仅在 launch_pid 仍存活时，按 exe 名重绑，且窗口必须属于 launch_pid 或其子进程，
    //    避免误绑其它同名实例（例如已开着一个 notepad 时再启动一个）。
    static HWND try_recover_main_window(const process_ops& proc,
                                        DWORD launch_pid,
                                        DWORD& io_capture_pid,
                                        const std::string& target_exe_base_name,
                                        bool allow_pid_rebind_by_exename,
                                        bool had_successful_video,
                                        uint64_t pid_rebind_deadline_unix_ms);


    // 判断是否应上报“远端已退出”：
    // 仅在曾成功出视频后，且窗口缺失持续超过 grace 窗口时才返回 true。
    static bool should_notify_remote_exit(bool had_successful_video,
                                          uint64_t now_ms,
                                          uint64_t& io_window_missing_since_unix_ms,
                                          uint32_t window_missing_exit_grace_ms);

private:
    // 仅提供静态方法：禁止实例化与拷贝，避免误用为“有状态对象”。
    SessionHealthPolicy() = delete;
    ~SessionHealthPolicy() = delete;
    SessionHealthPolicy(const SessionHealthPolicy&) = delete;
    SessionHealthPolicy& operator=(const SessionHealthPolicy&) = delete;

    static std::vector<HWND> find_all_windows(DWORD pid);

    static HWND find_window_by_exe_basename(const process_ops& proc,
        const std::string& exe_base_name,
        DWORD anchor_process_root = 0);

    static HWND find_window_by_exe_hint(const process_ops& proc,
        const std::string& exe_base_name,
        DWORD anchor_process_root = 0);

    static void log_window_candidates_for_rebind(const process_ops& proc,
        DWORD capture_pid,
        const std::string& target_exe_base_name);
};
