#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

class RemoteProcessSession;

// 进程控制器：统一管理远程进程生命周期、窗口发现、PID/HWND 重绑定与健康计时。
// 说明：
// - 作为“单一真相源（SSOT）”保存 launch_pid/capture_pid/main_hwnd 等权威状态
// - 对外提供 Snapshot（只读快照）与 Event（事件回调）
// - 当前阶段仅落地核心类，不接入 remote_video_engine；后续逐步迁移调用方。
class process_controller {
public:
    enum class state_t {
        Stopped,
        Starting,
        RunningHasWindow,
        RunningNoWindow,
        Exited,
        Stopping,
    };

    struct config_t {
        // 窗口缺失：超过该宽限才触发 WindowMissing 事件（需曾经成功出过视频）。
        uint32_t window_missing_exit_grace_ms = 5000;
        // 窗口缺失通知节流（避免刷屏/刷前端）。
        uint32_t window_missing_notify_throttle_ms = 2000;
        // 是否允许 PID 重绑定（按 exe basename / hint）。
        bool allow_pid_rebind_by_exename = true;
        // 仅在启动期启用 rebind 的窗口（ms）。0 表示不限制（不推荐）。
        uint32_t pid_rebind_startup_window_ms = 5000;
        // 运行期是否允许再次 rebind（后续策略增强用；当前默认 false 以保持行为保守）。
        bool allow_rebind_after_success = false;
    };

    struct snapshot_t {
        state_t st = state_t::Stopped;
        std::string exe_path;
        std::string target_exe_base_name;

        DWORD launch_pid = 0;
        DWORD capture_pid = 0;
        HWND main_hwnd = nullptr;

        bool had_successful_video = false;

        // 观测信息
        uint64_t now_unix_ms = 0;
        uint64_t pid_rebind_deadline_unix_ms = 0;
        uint64_t window_missing_since_unix_ms = 0;
        uint64_t last_window_missing_notify_unix_ms = 0;
        uint64_t last_window_seen_unix_ms = 0;

        // 最近一次窗口发现结果（用于诊断/策略判断）
        size_t last_surface_count = 0;
    };

    struct event_t {
        enum class type_t {
            Started,
            WindowRebound,
            WindowMissing,
            RemoteExited,
        } type = type_t::Started;

        // 通用字段
        uint64_t now_unix_ms = 0;
        const char* why = "";

        // Rebound
        DWORD old_pid = 0;
        DWORD new_pid = 0;
        bool by_hint = false;

        // Missing
        uint64_t missing_ms = 0;
    };

    using event_cb_t = std::function<void(const event_t&)>;

    process_controller();
    ~process_controller();

    process_controller(const process_controller&) = delete;
    process_controller& operator=(const process_controller&) = delete;

    void set_event_callback(event_cb_t cb);

    // Start/Stop：启动/停止远程进程与控制器线程
    bool start(std::string exe_path, config_t cfg);
    void stop();

    // 由外部（例如 capture/encode）反馈“曾经成功出过视频”，用于健康策略。
    void note_had_successful_video();

    // 轮询更新：窗口发现 / rebind / 缺失计时。可由调用方在自己的节拍中调用。
    // 若你更希望 controller 自己轮询，也可以启用内部 poll 线程（默认启用）。
    void poll_once();

    snapshot_t snapshot() const;

private:
    void emit_event_unlocked(event_t ev) const;
    void exit_watch_loop();
    void poll_loop();

    bool is_remote_process_still_running_unlocked() const;
    void update_window_discovery_unlocked(uint64_t now_unix_ms);
    bool should_throttle_window_missing_unlocked(uint64_t now_unix_ms) const;
    bool should_notify_window_missing_unlocked(uint64_t now_unix_ms);

private:
    mutable std::mutex m_mtx;
    event_cb_t m_cb;

    config_t m_cfg{};
    snapshot_t m_snap{};

    std::unique_ptr<RemoteProcessSession> m_session;
    PROCESS_INFORMATION m_pi{};

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_exit_watch_running{false};
    std::atomic<bool> m_poll_running{false};
    std::thread m_exit_watch_thread;
    std::thread m_poll_thread;
};

