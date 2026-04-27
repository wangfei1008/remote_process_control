#pragma once

// ============================================================
// win32_process.h
// Win32 进程操作：纯工具，无状态，所有方法均为 const
//
// 职责边界：
//   YES - OpenProcess / QueryFullProcessImageName / TerminateProcess
//   YES - Toolhelp 父链查询
//   NO  - 不持有任何 PID 或句柄状态
//   NO  - 不包含任何窗口或采集相关逻辑
// ============================================================

#include "win32_types.h"
#include <optional>
#include <string>

namespace win32 {

class Process {
public:
    // ---- 句柄操作 ----------------------------------------

    // 打开进程，返回 ScopedHandle（无效则 bool 为 false）
    ScopedHandle open(DWORD pid, DWORD access = PROCESS_QUERY_LIMITED_INFORMATION) const;

    // ---- 查询 --------------------------------------------

    // 返回完整可执行路径；失败返回 nullopt
    std::optional<std::string> query_image_path(DWORD pid) const;
    std::optional<std::string> query_image_path(HANDLE h)  const;

    // 返回小写 basename（如 "notepad.exe"）；失败返回空串
    std::string basename_lower(DWORD pid) const;

    // 进程是否仍在运行（基于 exit code）
    bool is_running(DWORD pid)    const;
    bool is_running(HANDLE h)     const;

    // 获取 exit code；失败返回 0
    DWORD exit_code(HANDLE h) const;

    // ---- 终止 --------------------------------------------

    bool terminate(DWORD pid,  UINT exit_code = 0) const;
    bool terminate(HANDLE h,   UINT exit_code = 0) const;

    // ---- Toolhelp 父链 -----------------------------------

    // 返回父进程 PID；失败返回 0
    DWORD parent_pid(DWORD pid) const;

    // pid 是否是 ancestor 的后代（含自身）
    bool is_descendant_of(DWORD pid, DWORD ancestor, int max_depth = 64) const;

    // ---- 工具 --------------------------------------------

    static std::string basename_from_path(const std::string& path);
    static std::string to_lower_ascii(std::string s);
};

} // namespace win32