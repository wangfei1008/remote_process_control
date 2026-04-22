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

    static bool is_window_viable_for_capture(HWND hwnd);


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

    // Intentionally minimal: keep this type focused on health thresholds and simple viability checks.
};
