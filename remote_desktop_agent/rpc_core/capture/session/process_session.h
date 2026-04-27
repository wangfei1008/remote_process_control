#pragma once

// ============================================================
// process_session.h
// 进程会话：生命周期管理
//
// 职责边界：
//   YES - CreateProcess / 句柄 RAII / start / stop
//   YES - 持有 launch_pid / capture_pid（语义状态）
//   NO  - 不暴露 OpenProcess / QueryFullImageName 等工具方法
//         （这些属于 win32::Process，调用方直接用）
//   NO  - 不包含窗口或采集相关逻辑
// ============================================================

#include "../infra/win32_types.h"
#include <string>

namespace capture {

// ----------------------------------------------------------
// ProcessLaunchConfig：启动参数（纯数据）
// ----------------------------------------------------------
struct ProcessLaunchConfig {
    std::string exe_path;
    DWORD       creation_flags = 0;
    bool        show_maximized = true;
};

// ----------------------------------------------------------
// DetachedHandles：转移给外部的裸句柄所有权
// （给需要裸 PROCESS_INFORMATION 的遗留代码使用）
// 调用方负责 CloseHandle
// ----------------------------------------------------------
struct DetachedHandles {
    PROCESS_INFORMATION pi{};
    DWORD               capture_pid = 0;
    DWORD               launch_pid  = 0;
};

// ----------------------------------------------------------
// ProcessSession
// ----------------------------------------------------------
class ProcessSession {
public:
    explicit ProcessSession(ProcessLaunchConfig cfg);
    ~ProcessSession();  // 析构自动调用 stop()

    ProcessSession(const ProcessSession&)            = delete;
    ProcessSession& operator=(const ProcessSession&) = delete;
    ProcessSession(ProcessSession&&)                 = default;
    ProcessSession& operator=(ProcessSession&&)      = default;

    // ---- 生命周期 ----------------------------------------

    // 启动进程；若已有进程先 stop
    bool start();

    struct StopOptions {bool terminate_launch  = true; bool terminate_capture = true; // capture_pid != launch_pid 时才生效
        UINT exit_code         = 0;
    };
    void stop(StopOptions opts = {});

    // launch 进程是否仍在运行
    bool is_launch_running() const;

    // ---- 状态读取 ----------------------------------------

    DWORD              launch_pid()             const { return m_launch_pid; }
    DWORD              capture_pid()            const { return m_capture_pid; }
    const std::string& target_basename_lower()  const { return m_target_basename_lower; }
    const std::string& exe_path()               const { return m_cfg.exe_path; }
    DWORD              launch_exit_code()       const;

    // ---- capture_pid 重绑 --------------------------------
    // 明确表达语义：调用方在 resolver 建议后，由此方法更新绑定
    void rebind_capture_pid(DWORD new_pid);

    // ---- 句柄转移（给遗留代码使用）----------------------
    // 转移后本对象保留 pid 语义，但不再持有句柄
    DetachedHandles detach_handles();

private:
    ProcessLaunchConfig  m_cfg;
    win32::ProcessInfoRaii m_pi;
    DWORD                m_launch_pid             = 0;
    DWORD                m_capture_pid            = 0;
    std::string          m_target_basename_lower;
};

} // namespace capture