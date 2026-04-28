// ============================================================
// process_session.cpp
// ============================================================

#include "process_session.h"
#include "../infra/win32_process.h"
#include <iostream>

namespace capture {

ProcessSession::ProcessSession(ProcessLaunchConfig cfg)
    : m_cfg(std::move(cfg))
{
    m_target_basename_lower = win32::Process::to_lower_ascii(win32::Process::basename_from_path(m_cfg.exe_path));
}

ProcessSession::~ProcessSession() {
    stop();
}

bool ProcessSession::start() {
#if defined(_WIN32)
    const std::string path = m_cfg.exe_path;
    stop({ true, true, 0 });
    m_cfg.exe_path = path;

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (m_cfg.show_maximized) {
        si.dwFlags     = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMAXIMIZED;
    }

    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(m_cfg.exe_path.c_str(),
                        nullptr, nullptr, nullptr,
                        FALSE, m_cfg.creation_flags,
                        nullptr, nullptr, &si, &pi)) {
        m_launch_pid  = 0;
        m_capture_pid = 0;
        return false;
    }

    // Create/assign job identity early so descendant/parent changes won't break session tracking.
    m_job_assigned = false;
    m_job.create(nullptr);
    if (m_job.valid()) {
        // Default: do not kill on close; keep semantics aligned with previous stop() behavior.
        // Can be changed later by config if needed.
        (void)m_job.set_kill_on_job_close(false);
        m_job_assigned = m_job.assign_process(pi.hProcess);
        if (!m_job_assigned) {
            // Keep running without job identity (fallback to legacy PID logic).
            m_job.reset();
        }
    }

    m_target_basename_lower = win32::Process::to_lower_ascii(win32::Process::basename_from_path(m_cfg.exe_path));
    m_pi.get()    = pi;
    m_launch_pid  = pi.dwProcessId;
    m_capture_pid = m_launch_pid;
    return true;
#else
    return false;
#endif
}

void ProcessSession::stop(StopOptions opts) {
#if defined(_WIN32)
    const DWORD saved_launch  = m_launch_pid;
    const DWORD saved_capture = m_capture_pid;

    win32::Process prims;

    if (opts.terminate_launch && m_pi.process_handle()) {
        prims.terminate(m_pi.process_handle(), opts.exit_code);
    }

    m_pi.reset();
    m_launch_pid  = 0;
    m_capture_pid = 0;
    m_target_basename_lower.clear();
    m_job.reset();
    m_job_assigned = false;
    // 保留 m_cfg.exe_path：与 legacy process_ops::terminate 语义对齐，允许同一会话对象再次 start()

    if (opts.terminate_capture
        && saved_capture != 0
        && saved_capture != saved_launch) {
        prims.terminate(saved_capture, opts.exit_code);
    }
#endif
}

bool ProcessSession::is_launch_running() const {
#if defined(_WIN32)
    return win32::Process{}.is_running(m_pi.process_handle());
#else
    return false;
#endif
}

DWORD ProcessSession::launch_exit_code() const {
#if defined(_WIN32)
    return win32::Process{}.exit_code(m_pi.process_handle());
#else
    return 0;
#endif
}

std::vector<DWORD> ProcessSession::session_pids() const
{
#if defined(_WIN32)
    if (has_job_identity()) {
		std::cout << "[proc] query session pids from job for launch_pid=" << m_launch_pid << std::endl;
        return m_job.query_process_ids();
    }
    std::vector<DWORD> out;
    if (m_capture_pid) out.push_back(m_capture_pid);
    if (m_launch_pid && m_launch_pid != m_capture_pid) out.push_back(m_launch_pid);
    return out;
#else
    return {};
#endif
}

bool ProcessSession::is_session_alive_with_pids(const DWORD* session_pids_data, size_t session_pids_count) const
{
#if defined(_WIN32)
    if (has_job_identity()) {
        return session_pids_count > 0;
    }
    if (is_launch_running()) {
        return true;
    }
    win32::Process prims;
    const size_t n = session_pids_data ? session_pids_count : 0;
    for (size_t i = 0; i < n; ++i) {
        const DWORD pid = session_pids_data[i];
        if (pid != 0 && prims.is_running(pid)) {
            return true;
        }
    }
    return false;
#else
    (void)session_pids_data;
    (void)session_pids_count;
    return false;
#endif
}

void ProcessSession::rebind_capture_pid(DWORD new_pid) {
    m_capture_pid = new_pid;
}

DetachedHandles ProcessSession::detach_handles() {
    DetachedHandles out;
    out.pi          = m_pi.detach();
    out.launch_pid  = m_launch_pid;
    out.capture_pid = m_capture_pid;
    m_launch_pid  = 0;
    m_capture_pid = 0;
    return out;
}

} // namespace capture