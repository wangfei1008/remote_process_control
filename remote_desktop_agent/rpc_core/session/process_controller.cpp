#include "session/process_controller.h"

#include "app/runtime_config.h"
#include "common/window_ops.h"
#include "common/rpc_time.h"
#include "session/remote_process_session.h"
#include "common/process_ops.h"
#include "session/session_health_policy.h"

#include <iostream>
#include <utility>

process_controller::process_controller()
{
    m_session = std::make_unique<RemoteProcessSession>();
}

process_controller::~process_controller()
{
    stop();
}

void process_controller::set_event_callback(event_cb_t cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_cb = std::move(cb);
}

bool process_controller::start(std::string exe_path, config_t cfg)
{
    stop();

    std::lock_guard<std::mutex> lk(m_mtx);
    m_cfg = cfg;
    m_snap = snapshot_t{};
    m_snap.st = state_t::Starting;
    m_snap.exe_path = std::move(exe_path);

    m_pi = {};
    DWORD launch_pid = 0;
    DWORD capture_pid = 0;
    HWND main_hwnd = nullptr;
    std::string target_base;

    if (!m_session || !m_session->launch_process(m_snap.exe_path, m_pi, launch_pid, capture_pid, main_hwnd, target_base)) {
        m_snap.st = state_t::Stopped;
        return false;
    }

    m_snap.launch_pid = launch_pid;
    m_snap.capture_pid = capture_pid;
    m_snap.main_hwnd = main_hwnd;
    m_snap.target_exe_base_name = target_base;
    m_snap.pid_rebind_deadline_unix_ms = rpc_unix_epoch_ms() + (uint64_t)m_cfg.pid_rebind_startup_window_ms;

    m_running.store(true, std::memory_order_release);

    // Threads
    m_exit_watch_running.store(true, std::memory_order_release);
    m_exit_watch_thread = std::thread(&process_controller::exit_watch_loop, this);

    m_poll_running.store(true, std::memory_order_release);
    m_poll_thread = std::thread(&process_controller::poll_loop, this);

    emit_event_unlocked(event_t{ event_t::type_t::Started, rpc_unix_epoch_ms(), "started" });
    return true;
}

void process_controller::stop()
{
    m_running.store(false, std::memory_order_release);
    m_exit_watch_running.store(false, std::memory_order_release);
    m_poll_running.store(false, std::memory_order_release);

    if (m_exit_watch_thread.joinable()) m_exit_watch_thread.join();
    if (m_poll_thread.joinable()) m_poll_thread.join();

    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_pi.hProcess) {
        try {
            m_snap.st = state_t::Stopping;
            std::cout << "[proc_ctl] stop: terminating launch_pid=" << m_snap.launch_pid
                      << " capture_pid=" << m_snap.capture_pid << std::endl;
            if (m_session) {
                m_session->terminate_processes(m_pi, m_snap.capture_pid, m_snap.launch_pid);
            } else {
                process_ops ops;
                if (m_pi.hProcess) {
                    ops.terminate_by_handle(m_pi.hProcess, 0);
                    CloseHandle(m_pi.hProcess);
                    m_pi.hProcess = nullptr;
                }
                if (m_pi.hThread) {
                    CloseHandle(m_pi.hThread);
                    m_pi.hThread = nullptr;
                }
                if (m_snap.capture_pid != 0 && m_snap.capture_pid != m_snap.launch_pid) {
                    ops.terminate_by_pid(m_snap.capture_pid, 0);
                }
            }
        } catch (...) {
        }
    }
    m_pi = {};
    m_snap.st = state_t::Stopped;
}

void process_controller::note_had_successful_video()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    m_snap.had_successful_video = true;
}

process_controller::snapshot_t process_controller::snapshot() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    snapshot_t s = m_snap;
    s.now_unix_ms = rpc_unix_epoch_ms();
    return s;
}

void process_controller::poll_once()
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_running.load(std::memory_order_relaxed)) return;
    const uint64_t now = rpc_unix_epoch_ms();
    m_snap.now_unix_ms = now;
    update_window_discovery_unlocked(now);
}

void process_controller::poll_loop()
{
    while (m_poll_running.load(std::memory_order_acquire)) {
        poll_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void process_controller::exit_watch_loop()
{
    while (m_exit_watch_running.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (!m_running.load(std::memory_order_relaxed)) break;
            if (!is_remote_process_still_running_unlocked()) {
                m_snap.st = state_t::Exited;
                emit_event_unlocked(event_t{ event_t::type_t::RemoteExited, rpc_unix_epoch_ms(), "process_not_running" });
                m_running.store(false, std::memory_order_release);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    m_exit_watch_running.store(false, std::memory_order_release);
}

bool process_controller::is_remote_process_still_running_unlocked() const
{
    if (m_pi.hProcess) {
        DWORD exit_code = 0;
        if (GetExitCodeProcess(m_pi.hProcess, &exit_code) && exit_code == STILL_ACTIVE) {
            return true;
        }
    }
    const DWORD cap = m_snap.capture_pid;
    const DWORD launch = m_snap.launch_pid;
    process_ops ops;
    if (cap != 0 && ops.is_running(cap)) return true;
    if (launch != 0 && ops.is_running(launch)) return true;
    return false;
}

bool process_controller::should_throttle_window_missing_unlocked(uint64_t now_unix_ms) const
{
    if (m_snap.last_window_missing_notify_unix_ms == 0) return false;
    if (now_unix_ms < m_snap.last_window_missing_notify_unix_ms) return true;
    return (now_unix_ms - m_snap.last_window_missing_notify_unix_ms) < (uint64_t)m_cfg.window_missing_notify_throttle_ms;
}

bool process_controller::should_notify_window_missing_unlocked(uint64_t now_unix_ms)
{
    // Mirror existing semantics: only start counting after successful video.
    if (m_snap.window_missing_since_unix_ms == 0) {
        m_snap.window_missing_since_unix_ms = now_unix_ms;
    }
    if (!m_snap.had_successful_video) return false;
    if (now_unix_ms < m_snap.window_missing_since_unix_ms) return false;
    return (now_unix_ms - m_snap.window_missing_since_unix_ms) >= (uint64_t)m_cfg.window_missing_exit_grace_ms;
}

void process_controller::update_window_discovery_unlocked(uint64_t now_unix_ms)
{
    if (!m_session) return;

    // Window discovery / rebind policy.
    // Keep current behavior conservative by default: rebind only before first successful video.
    const bool allow_rebind_now =
        m_cfg.allow_pid_rebind_by_exename &&
        (!m_snap.had_successful_video || m_cfg.allow_rebind_after_success) &&
        !m_snap.target_exe_base_name.empty() &&
        now_unix_ms <= m_snap.pid_rebind_deadline_unix_ms &&
        m_snap.launch_pid != 0 &&
        (process_ops().is_running(m_snap.launch_pid));

    if (!m_snap.main_hwnd || !m_session->is_window_viable_for_capture(m_snap.main_hwnd)) {
        m_snap.main_hwnd = nullptr;
    }

    // Use existing policy helper for now (behavior compatible).
    if (allow_rebind_now) {
        DWORD old = m_snap.capture_pid;
        HWND hwnd = SessionHealthPolicy::try_recover_main_window(
            *m_session,
            m_snap.launch_pid,
            m_snap.capture_pid,
            m_snap.target_exe_base_name,
            m_cfg.allow_pid_rebind_by_exename,
            m_snap.had_successful_video,
            m_snap.pid_rebind_deadline_unix_ms);

        if (hwnd && old != m_snap.capture_pid) {
            event_t ev;
            ev.type = event_t::type_t::WindowRebound;
            ev.now_unix_ms = now_unix_ms;
            ev.why = "rebind_by_exe_or_hint";
            ev.old_pid = old;
            ev.new_pid = m_snap.capture_pid;
            ev.by_hint = true; // best-effort; policy prints exact info elsewhere
            emit_event_unlocked(ev);
        }
        if (hwnd) m_snap.main_hwnd = hwnd;
    }

    // Surfaces discovery (lightweight count) for health state; detailed capture is done elsewhere.
    window_ops wops;
    const auto surfaces = wops.enumerate_visible_top_level(m_snap.capture_pid);
    m_snap.last_surface_count = surfaces.size();

    if (!surfaces.empty()) {
        m_snap.last_window_seen_unix_ms = now_unix_ms;
        m_snap.window_missing_since_unix_ms = 0;
        if (m_snap.st != state_t::RunningHasWindow) {
            m_snap.st = state_t::RunningHasWindow;
        }
        return;
    }

    // No surfaces
    if (m_snap.st != state_t::RunningNoWindow) {
        m_snap.st = state_t::RunningNoWindow;
    }

    if (should_notify_window_missing_unlocked(now_unix_ms) && !should_throttle_window_missing_unlocked(now_unix_ms)) {
        m_snap.last_window_missing_notify_unix_ms = now_unix_ms;
        event_t ev;
        ev.type = event_t::type_t::WindowMissing;
        ev.now_unix_ms = now_unix_ms;
        ev.why = "no_surfaces_grace_expired_but_process_alive";
        const uint64_t since = m_snap.window_missing_since_unix_ms ? m_snap.window_missing_since_unix_ms : now_unix_ms;
        ev.missing_ms = (now_unix_ms >= since) ? (now_unix_ms - since) : 0;
        emit_event_unlocked(ev);
    }
}

void process_controller::emit_event_unlocked(event_t ev) const
{
    if (!m_cb) return;
    try {
        m_cb(ev);
    } catch (...) {
    }
}

