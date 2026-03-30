#include "process_manager.h"
#include "input_controller.h"
#include "rpc_time.h"
#include "capture_rgb_heuristics.h"
#include "h264_avcc_utils.h"
#include <cstdint>
#include <vector>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <cstring>

static bool rpc_probe_dxgi_capture_support()
{
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    const bool ok = (dxgi != nullptr && d3d11 != nullptr);
    if (dxgi) FreeLibrary(dxgi);
    if (d3d11) FreeLibrary(d3d11);
    return ok;
}

bool ProcessManager::should_apply_layout_change(int capturedW, int capturedH)
{
    if (capturedW <= 0 || capturedH <= 0) return false;
    if (m_width <= 0 || m_height <= 0) return true;
    // Once we have produced valid video, lock encoder resolution to avoid
    // frontend/cpp_receiver letterbox recalculation causing visible flicker.
    if (m_had_successful_video) return false;

    const int dw = std::abs(capturedW - m_width);
    const int dh = std::abs(capturedH - m_height);
    const bool diffEnough = (dw > m_layout_change_threshold_px) || (dh > m_layout_change_threshold_px);
    if (!diffEnough) {
        reset_layout_change_tracking();
        return false;
    }

    if (m_pending_layout_w != capturedW || m_pending_layout_h != capturedH) {
        m_pending_layout_w = capturedW;
        m_pending_layout_h = capturedH;
        m_layout_change_streak = 1;
    } else {
        ++m_layout_change_streak;
    }

    if (m_layout_change_streak >= m_layout_change_required_streak) {
        // Caller will recreate encoder and update m_width/m_height.
        reset_layout_change_tracking();
        return true;
    }
    return false;
}

void ProcessManager::reset_layout_change_tracking()
{
    m_layout_change_streak = 0;
    m_pending_layout_w = 0;
    m_pending_layout_h = 0;
}

ProcessManager::ProcessManager() 
    : m_width(1536)
    , m_height(864)
    , m_fps(30)
    , m_active_fps(30)
    , m_idle_fps(5)
    , m_current_fps(30)
    , running(false)
    , sampleTime_us(0)
{
    ZeroMemory(&m_pi, sizeof(m_pi));
    if (const char* envCaptureAll = std::getenv("RPC_CAPTURE_ALL_WINDOWS")) {
        m_capture_all_windows = (std::atoi(envCaptureAll) != 0);
    }
    if (const char* envIdleFps = std::getenv("RPC_IDLE_FPS")) {
        m_idle_fps = (std::max)(1, std::atoi(envIdleFps));
    }
    if (const char* envActiveFps = std::getenv("RPC_ACTIVE_FPS")) {
        m_active_fps = (std::max)(1, std::atoi(envActiveFps));
    }
    if (m_idle_fps > m_active_fps) m_idle_fps = m_active_fps;
    m_fps = m_active_fps;
    m_current_fps = m_active_fps;

    m_hw_capture_supported = (rpc_probe_dxgi_capture_support() && m_dxgiCapture.is_available());
    if (const char* envCaptureBackend = std::getenv("RPC_CAPTURE_BACKEND")) {
        std::string mode = envCaptureBackend;
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mode == "dxgi") {
            m_hw_capture_requested = true;
            m_lock_capture_backend = true;
            m_locked_use_hw_capture = true;
        } else if (mode == "gdi") {
            m_hw_capture_requested = false;
            m_lock_capture_backend = true;
            m_locked_use_hw_capture = false;
        } else {
            m_hw_capture_requested = m_hw_capture_supported; // auto
            m_lock_capture_backend = false;
        }
    } else {
        m_hw_capture_requested = m_hw_capture_supported; // default auto
        m_lock_capture_backend = false;
    }
    if (const char* envRebind = std::getenv("RPC_ALLOW_CAPTURE_PID_REBIND")) {
        m_allow_pid_rebind_by_exename = (std::atoi(envRebind) != 0);
    }
    m_hw_capture_active = (m_hw_capture_requested && m_hw_capture_supported);
    std::cout << "[capture] dxgi_supported=" << (m_hw_capture_supported ? 1 : 0)
              << " requested=" << (m_hw_capture_requested ? 1 : 0)
              << " active=" << (m_hw_capture_active ? 1 : 0)
              << " lock=" << (m_lock_capture_backend ? 1 : 0)
              << " locked_backend=" << (m_locked_use_hw_capture ? "dxgi" : "gdi")
              << " mode=" << (m_capture_all_windows ? "all-windows" : "main-window")
              << std::endl;
}

ProcessManager::~ProcessManager()
{
    terminate();
}

std::string ProcessManager::basename_from_path(const std::string& path)
{
    const auto pos1 = path.find_last_of("\\/");
    const std::string name = (pos1 == std::string::npos) ? path : path.substr(pos1 + 1);
    return name;
}

std::string ProcessManager::get_process_basename(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    char buf[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    std::string out;
    if (QueryFullProcessImageNameA(h, 0, buf, &size)) {
        out = basename_from_path(std::string(buf, buf + size));
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    }
    CloseHandle(h);
    return out;
}

HWND ProcessManager::find_window_by_exe_basename(const std::string& exeBaseName)
{
    if (exeBaseName.empty()) return nullptr;
    const std::string target = [&]{
        std::string t = exeBaseName;
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return t;
    }();

    struct Best {
        HWND hwnd = nullptr;
        int area = 0;
    } best;

    struct EnumCtx {
        const char* target;
        Best* best;
    } ctx{ target.c_str(), &best };

    EnumWindows([](HWND hwnd, LPARAM lp)->BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

        RECT r{};
        if (!GetWindowRect(hwnd, &r)) return TRUE;
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;
        if (w <= 0 || h <= 0) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return TRUE;

        std::string base = ProcessManager::get_process_basename(pid);
        if (base.empty()) return TRUE;

        if (base != c->target) return TRUE;

        const int area = w * h;
        if (area > c->best->area) {
            c->best->area = area;
            c->best->hwnd = hwnd;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return best.hwnd;
}

HWND ProcessManager::launch_process(const std::string& exe_path) 
{
    STARTUPINFO si = { sizeof(si) };
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    m_targetExeBaseName = basename_from_path(exe_path);
    std::transform(m_targetExeBaseName.begin(), m_targetExeBaseName.end(), m_targetExeBaseName.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });

    if (!CreateProcessA(exe_path.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &m_pi)) {
        std::cout << "[proc] CreateProcess failed, exe=" << exe_path
                  << " error=" << GetLastError() << std::endl;
        return nullptr;
    }

    std::cout << "[proc] CreateProcess ok, pid=" << m_pi.dwProcessId
              << " exe=" << exe_path << std::endl;

    m_launchPid = m_pi.dwProcessId;
    m_capturePid = m_launchPid;

    // Do not block stream creation waiting for window.
    // Window will be discovered lazily in load_next_sample().
    m_mainWindow = find_main_window(m_pi.dwProcessId);
    if (!m_mainWindow) {
        auto wins = find_all_windows(m_pi.dwProcessId);
        if (!wins.empty()) m_mainWindow = wins.front();
    }
    // Use client rect to reduce 1~2px jitter caused by non-client area (borders/shadows).
    RECT clientRc{};
    if (GetClientRect(m_mainWindow, &clientRc)) {
        POINT tl{ clientRc.left, clientRc.top };
        POINT br{ clientRc.right, clientRc.bottom };
        if (ClientToScreen(m_mainWindow, &tl) && ClientToScreen(m_mainWindow, &br)) {
            m_width = br.x - tl.x;
            m_height = br.y - tl.y;
        }
    }

    // Enforce even dimensions for YUV420 encoding.
    if (m_width > 0 && m_height > 0) {
        m_width = (m_width + 1) & ~1;
        m_height = (m_height + 1) & ~1; // keep height even
    }
    m_av_codec_ctx = create_h264_encoder(m_width, m_height, m_fps);
    m_extradata_spspps = parse_avcc_spspps(m_av_codec_ctx);
    std::cout << "[proc] launch_process done, hasWindow=" << (m_mainWindow ? 1 : 0) << std::endl;
    return m_mainWindow;
}

void ProcessManager::terminate() 
{
    if (m_pi.hProcess)
    {
        TerminateProcess(m_pi.hProcess, 0);
        CloseHandle(m_pi.hProcess);
        CloseHandle(m_pi.hThread);
        m_pi.hProcess = nullptr;
        m_pi.hThread = nullptr;
    }

    // If we rebound to a different PID (window owned by another process), terminate it too.
    if (m_capturePid != 0 && m_capturePid != m_launchPid) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m_capturePid);
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
        }
    }
    m_mainWindow = nullptr;
    running = false;
}

void ProcessManager::start()
{
    running = true;
    m_had_successful_video = false;
    m_exit_notified = false;
    m_pid_rebind_deadline_unix_ms = rpc_unix_epoch_ms() + 15000; // allow PID rebound only in startup window
    m_window_missing_since_unix_ms = 0;
    sampleTime_us = 0;
    m_encode_frame_seq = 0;
    m_stable_frame_count = 0;
    m_has_last_sig = false;
    m_last_frame_sig = 0;
    m_current_fps = m_active_fps;
    reset_layout_change_tracking();
    m_last_good_sample.clear();
    m_have_last_good_sample = false;
    m_last_good_rgb_frame.clear();
    m_last_good_rgb_w = 0;
    m_last_good_rgb_h = 0;
    m_pending_force_keyframe.store(false);
    m_last_force_keyframe_unix_ms.store(0, std::memory_order_relaxed);
    m_force_software_capture_until_unix_ms.store(0, std::memory_order_relaxed);
    m_dxgi_fail_streak = 0;
    m_top_black_strip_streak = 0;
    m_disable_dxgi_for_session = false;
    m_last_capture_used_hw = false;
    m_dxgi_instability_score = 0;
}

void ProcessManager::stop()
{
    // Strong lifecycle guarantee:
    // when upper layer stops stream (frontend closed stream/page),
    // the launched process must be terminated and resources released.
    terminate();
}

void ProcessManager::request_force_keyframe()
{
    // Best-effort recovery for packet loss/jitter: make next encoded frame an IDR/keyframe.
    request_force_keyframe_with_cooldown();
}

void ProcessManager::request_force_keyframe_with_cooldown()
{
    // Cooldown prevents repeatedly forcing IDR during sustained capture glitches.
    // Frequent IDR forcing can trigger decoder spikes, showing as flicker/black bars.
    constexpr uint64_t kCooldownMs = 3000; // 3s

    const uint64_t nowMs = rpc_unix_epoch_ms();
    while (true) {
        const uint64_t lastAllowedMs = m_last_force_keyframe_unix_ms.load(std::memory_order_relaxed);
        if (lastAllowedMs != 0 && nowMs >= lastAllowedMs && (nowMs - lastAllowedMs) < kCooldownMs) {
            return; // still in cooldown window
        }

        uint64_t expected = lastAllowedMs;
        if (m_last_force_keyframe_unix_ms.compare_exchange_weak(
                expected, nowMs, std::memory_order_relaxed, std::memory_order_relaxed)) {
            m_pending_force_keyframe.store(true, std::memory_order_relaxed);
            return;
        }
        // CAS failed due to race; retry.
    }
}

void ProcessManager::notify_remote_exit()
{
    if (m_exit_notified)
        return;
    m_exit_notified = true;
    running = false;
    std::cout << "[proc] remote application window/process ended, notifying viewers" << std::endl;
    if (m_on_remote_exit) {
        try {
            m_on_remote_exit();
        } catch (...) {
        }
    }
}

uint64_t ProcessManager::quick_frame_signature(const std::vector<uint8_t>& frame, int width, int height) const
{
    if (frame.empty() || width <= 0 || height <= 0) return 0;
    const int bytesPerPixel = 3;
    const size_t stride = static_cast<size_t>(width) * bytesPerPixel;
    const int sampleRows = (std::min)(height, 32);
    const int sampleCols = (std::min)(width, 64);
    const int rowStep = (std::max)(1, height / sampleRows);
    const int colStep = (std::max)(1, width / sampleCols);
    uint64_t sig = 1469598103934665603ull; // FNV-1a basis
    for (int y = 0; y < height; y += rowStep) {
        const size_t rowBase = static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; x += colStep) {
            const size_t idx = rowBase + static_cast<size_t>(x) * bytesPerPixel;
            if (idx + 2 >= frame.size()) break;
            sig ^= static_cast<uint64_t>(frame[idx + 0]); sig *= 1099511628211ull;
            sig ^= static_cast<uint64_t>(frame[idx + 1]); sig *= 1099511628211ull;
            sig ^= static_cast<uint64_t>(frame[idx + 2]); sig *= 1099511628211ull;
        }
    }
    return sig;
}

std::vector<uint8_t> ProcessManager::capture_main_window_image(HWND hwnd, int& outWidth, int& outHeight, int& outMinLeft, int& outMinTop)
{
    outWidth = 0;
    outHeight = 0;
    outMinLeft = 0;
    outMinTop = 0;
    if (!hwnd || !IsWindow(hwnd)) return {};
    RECT clientRc{};
    if (!GetClientRect(hwnd, &clientRc)) return {};

    POINT tl{ clientRc.left, clientRc.top };
    POINT br{ clientRc.right, clientRc.bottom };
    if (!ClientToScreen(hwnd, &tl) || !ClientToScreen(hwnd, &br)) return {};

    const int width = br.x - tl.x;
    const int height = br.y - tl.y;
    if (width <= 0 || height <= 0) return {};
    auto frame = m_windowCapture.capture(hwnd, width, height);
    if (frame.empty()) return {};
    outWidth = width & ~1;
    outHeight = height & ~1;
    outMinLeft = tl.x;
    outMinTop = tl.y;
    if (outWidth <= 0 || outHeight <= 0) return {};
    if (outWidth != width || outHeight != height) {
        std::vector<uint8_t> cropped(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 3u, 0);
        for (int y = 0; y < outHeight; ++y) {
            const size_t src = static_cast<size_t>(y) * static_cast<size_t>(width) * 3u;
            const size_t dst = static_cast<size_t>(y) * static_cast<size_t>(outWidth) * 3u;
            std::memcpy(cropped.data() + dst, frame.data() + src, static_cast<size_t>(outWidth) * 3u);
        }
        frame.swap(cropped);
    }
    return frame;
}

bool ProcessManager::tick_window_and_health(std::chrono::steady_clock::time_point& last_no_window_diag)
{
    if (m_had_successful_video && !m_exit_notified) {
        if (m_capturePid != 0) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m_capturePid);
            if (hProc) {
                DWORD code = STILL_ACTIVE;
                if (GetExitCodeProcess(hProc, &code) && code != STILL_ACTIVE) {
                    m_mainWindow = nullptr;
                }
                CloseHandle(hProc);
            }
        }
        if (m_mainWindow && !IsWindow(m_mainWindow)) {
            m_mainWindow = nullptr;
        }
    }

    if (!m_mainWindow) {
        m_mainWindow = find_main_window(m_capturePid);
        if (!m_mainWindow) {
            std::vector<HWND> wins = find_all_windows(m_capturePid);
            for (HWND h : wins) {
                if (IsWindowVisible(h)) {
                    m_mainWindow = h;
                    break;
                }
            }
            if (!m_mainWindow && !wins.empty())
                m_mainWindow = wins.front();
        }

        if (m_allow_pid_rebind_by_exename &&
            !m_had_successful_video &&
            !m_mainWindow &&
            !m_targetExeBaseName.empty() &&
            rpc_unix_epoch_ms() <= m_pid_rebind_deadline_unix_ms) {
            HWND h = find_window_by_exe_basename(m_targetExeBaseName);
            if (h) {
                DWORD realPid = 0;
                GetWindowThreadProcessId(h, &realPid);
                if (realPid) {
                    m_capturePid = realPid;
                    m_mainWindow = h;
                }
            }
        }

        if (!m_mainWindow) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_no_window_diag > std::chrono::seconds(1)) {
                last_no_window_diag = now;
                std::vector<HWND> wins = find_all_windows(m_capturePid);
                std::cout << "[proc] no window yet, pid=" << m_capturePid
                          << " windows=" << wins.size() << std::endl;
            }
            sample.clear();

            const uint64_t nowMs = rpc_unix_epoch_ms();
            if (m_window_missing_since_unix_ms == 0) {
                m_window_missing_since_unix_ms = nowMs;
            }
            if (m_had_successful_video &&
                nowMs >= m_window_missing_since_unix_ms &&
                (nowMs - m_window_missing_since_unix_ms) >= static_cast<uint64_t>(m_window_missing_exit_grace_ms)) {
                notify_remote_exit();
                return false;
            }
            sampleTime_us += get_sample_duration_us();
            return false;
        }
        m_pid_rebind_deadline_unix_ms = 0;
        m_window_missing_since_unix_ms = 0;
    }
    return true;
}

void ProcessManager::emit_hold_or_empty_sample(bool request_idr)
{
    const bool steadyFrameHold = (m_lock_capture_backend && !m_locked_use_hw_capture);
    if (steadyFrameHold && m_have_last_good_sample) {
        sample = m_last_good_sample;
        if (request_idr) request_force_keyframe_with_cooldown();
    } else {
        sample.clear();
    }
}

bool ProcessManager::decide_use_hw_capture(uint64_t cap_now_ms)
{
    const uint64_t forceSwUntil = m_force_software_capture_until_unix_ms.load(std::memory_order_relaxed);
    bool useHwCapture = false;
    if (m_lock_capture_backend) {
        useHwCapture = m_hw_capture_supported && m_locked_use_hw_capture;
    } 
    else {
        useHwCapture = m_hw_capture_active && !m_disable_dxgi_for_session && (cap_now_ms >= forceSwUntil);
    }
    m_last_capture_used_hw = useHwCapture;
    return useHwCapture;
}

bool ProcessManager::grab_rgb_frame(int& width, int& height, int& cap_min_left, int& cap_min_top, std::vector<uint8_t>& frame, bool use_hw_capture)
{
    if (use_hw_capture) {
        if (m_capture_all_windows) {
            frame = capture_all_windows_image(m_capturePid, m_mainWindow, 1024, width, height, cap_min_left, cap_min_top);
        } 
        else {
            frame = m_dxgiCapture.capture_window_rgb(m_mainWindow, width, height, cap_min_left, cap_min_top);
            if (frame.empty()) {
                m_dxgi_fail_streak++;
                m_dxgi_instability_score = (std::min)(m_dxgi_instability_score + 2, 1000);
                if (m_dxgi_fail_streak >= m_dxgi_fail_reset_threshold) {
                    m_dxgiCapture.reset();
                    m_dxgi_fail_streak = 0;
                    std::cout << "[capture] dxgi repeated empty, reset duplication\n";
                }
                static auto lastDxgiFailLog = std::chrono::steady_clock::now();
                const auto now = std::chrono::steady_clock::now();
                if (now - lastDxgiFailLog > std::chrono::seconds(1)) {
                    lastDxgiFailLog = now;
                    std::cout << "[capture] dxgi returned empty\n";
                }
                if (!m_lock_capture_backend) {
                    m_force_software_capture_until_unix_ms.store(
                        rpc_unix_epoch_ms() + static_cast<uint64_t>(m_capture_degrade_ms),
                        std::memory_order_relaxed);
                    if (!m_disable_dxgi_for_session &&
                        m_dxgi_instability_score >= m_dxgi_disable_score_threshold) {
                        m_disable_dxgi_for_session = true;
                        std::cout << "[capture] dxgi disabled for this session due to repeated instability score="
                                  << m_dxgi_instability_score << "\n";
                    }
                }
                if (!m_lock_capture_backend) {
                    frame = capture_main_window_image(m_mainWindow, width, height, cap_min_left, cap_min_top);
                    if (frame.empty() && m_had_successful_video && m_have_last_good_sample) {
                        emit_hold_or_empty_sample(true);
                        sampleTime_us += get_sample_duration_us();
                        return false;
                    }
                }
            } else {
                m_dxgi_fail_streak = 0;
                if (m_dxgi_instability_score > 0) {
                    m_dxgi_instability_score = (std::max)(0, m_dxgi_instability_score - 1);
                }
            }
        }
    } 
    else if (m_capture_all_windows) {
        frame = capture_all_windows_image(m_capturePid, m_mainWindow, 1024, width, height, cap_min_left, cap_min_top);
    } else {
        frame = capture_main_window_image(m_mainWindow, width, height, cap_min_left, cap_min_top);
    }
    return true;
}

bool ProcessManager::discard_if_capture_too_slow(std::chrono::steady_clock::time_point t_cap_begin,
                                                 std::chrono::steady_clock::time_point t_after_cap,
                                                 bool use_hw_capture)
{
    if (!(m_had_successful_video && m_have_last_good_sample))
        return false;
    const auto capMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t_after_cap - t_cap_begin).count());
    constexpr int kMaxCaptureMs = 80;
    if (capMs < kMaxCaptureMs)
        return false;

    static auto lastSlowLog = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastSlowLog > std::chrono::seconds(1)) {
        lastSlowLog = now;
        std::cout << "[capture] slow capture " << capMs << "ms, discard frame\n";
    }
    if (use_hw_capture && !m_lock_capture_backend) {
        m_dxgi_instability_score = (std::min)(m_dxgi_instability_score + 1, 1000);
        m_force_software_capture_until_unix_ms.store(
            rpc_unix_epoch_ms() + static_cast<uint64_t>(m_capture_degrade_ms), std::memory_order_relaxed);
        if (!m_disable_dxgi_for_session &&
            m_dxgi_instability_score >= m_dxgi_disable_score_threshold) {
            m_disable_dxgi_for_session = true;
            std::cout << "[capture] dxgi disabled for this session due to repeated slow-capture score="
                      << m_dxgi_instability_score << "\n";
        }
    }
    emit_hold_or_empty_sample(false);
    sampleTime_us += get_sample_duration_us();
    m_last_capture_ms = static_cast<uint32_t>(capMs);
    m_last_encode_ms = 0;
    return true;
}

bool ProcessManager::discard_if_empty_frame(const std::vector<uint8_t>& frame, int width, int height)
{
    if (!frame.empty() && width > 0 && height > 0)
        return false;
    emit_hold_or_empty_sample(true);
    sampleTime_us += get_sample_duration_us();
    return true;
}

bool ProcessManager::filter_suspicious_frame(std::vector<uint8_t>& frame, int& width, int& height, int& cap_min_left,
                                            int& cap_min_top)
{
    if (!(m_had_successful_video && m_have_last_good_sample))
        return true;

    const bool hasTopBlackStrip = capture_rgb::frame_has_top_black_strip_rgb24(frame, width, height);
    bool suspicious = capture_rgb::is_suspicious_capture_frame(frame, width, height);
    if (hasTopBlackStrip) {
        const bool canRepairWithPrev =
            (!m_last_good_rgb_frame.empty() &&
             m_last_good_rgb_w == width &&
             m_last_good_rgb_h == height &&
             m_last_good_rgb_frame.size() == frame.size());
        if (canRepairWithPrev) {
            capture_rgb::repair_top_strip_from_previous(frame, width, height, m_last_good_rgb_frame);
            suspicious = capture_rgb::is_suspicious_capture_frame(frame, width, height);
        }
        ++m_top_black_strip_streak;
        if (m_hw_capture_active) {
            constexpr uint64_t kLongForceSwMs = 8000;
            m_force_software_capture_until_unix_ms.store(
                rpc_unix_epoch_ms() + kLongForceSwMs, std::memory_order_relaxed);
        }
        if (!m_disable_dxgi_for_session &&
            m_top_black_strip_streak >= m_top_black_strip_disable_dxgi_threshold) {
            m_disable_dxgi_for_session = true;
            std::cout << "[capture] dxgi disabled for this session due to repeated top black strip\n";
        }
    } else if (m_top_black_strip_streak > 0) {
        --m_top_black_strip_streak;
    }

    if (suspicious && m_hw_capture_active && !m_capture_all_windows && m_mainWindow && IsWindow(m_mainWindow)) {
        int sw = 0, sh = 0, sl = 0, st = 0;
        auto fallbackFrame = capture_main_window_image(m_mainWindow, sw, sh, sl, st);
        if (!fallbackFrame.empty() && sw > 0 && sh > 0 &&
            !capture_rgb::is_suspicious_capture_frame(fallbackFrame, sw, sh)) {
            frame.swap(fallbackFrame);
            width = sw;
            height = sh;
            cap_min_left = sl;
            cap_min_top = st;
            suspicious = false;
            static auto lastRecoverLog = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - lastRecoverLog > std::chrono::seconds(1)) {
                lastRecoverLog = now;
                std::cout << "[capture] suspicious dxgi frame recovered by software recapture\n";
            }
        }
    }

    if (suspicious && m_hw_capture_active && !m_capture_all_windows && m_mainWindow && IsWindow(m_mainWindow) &&
        m_top_black_strip_streak >= m_top_black_strip_force_sw_threshold) {
        int sw = 0, sh = 0, sl = 0, st = 0;
        auto swFrame = capture_main_window_image(m_mainWindow, sw, sh, sl, st);
        if (!swFrame.empty() && sw > 0 && sh > 0 &&
            !capture_rgb::is_suspicious_capture_frame(swFrame, sw, sh)) {
            frame.swap(swFrame);
            width = sw;
            height = sh;
            cap_min_left = sl;
            cap_min_top = st;
            suspicious = false;
        }
    }

    if (suspicious) {
        static auto lastBadLog = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastBadLog > std::chrono::seconds(1)) {
            lastBadLog = now;
            std::cout << "[capture] suspicious frame detected, discard frame\n";
        }
        emit_hold_or_empty_sample(true);
        sampleTime_us += get_sample_duration_us();
        return false;
    }
    return true;
}

void ProcessManager::ensure_encoder_layout(int captured_w, int captured_h, bool& applied_layout_out)
{
    applied_layout_out = false;
    if (!m_av_codec_ctx) {
        m_width = captured_w;
        m_height = captured_h;
        m_av_codec_ctx = create_h264_encoder(m_width, m_height, m_fps);
        m_extradata_spspps = parse_avcc_spspps(m_av_codec_ctx);
        m_encode_frame_seq = 0;
        reset_layout_change_tracking();
        applied_layout_out = true;
    } else if (should_apply_layout_change(captured_w, captured_h)) {
        if (m_av_codec_ctx) {
            destroy_h264_encoder(m_av_codec_ctx);
            m_av_codec_ctx = nullptr;
        }
        m_width = captured_w;
        m_height = captured_h;
        m_encode_frame_seq = 0;
        m_av_codec_ctx = create_h264_encoder(m_width, m_height, m_fps);
        m_extradata_spspps = parse_avcc_spspps(m_av_codec_ctx);
        applied_layout_out = true;
    }

    if (applied_layout_out) {
        static auto lastLayoutLog = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastLayoutLog > std::chrono::seconds(1)) {
            lastLayoutLog = now;
            std::cout << "[layout] encoder updated: captured=" << captured_w << "x" << captured_h
                      << " -> target=" << m_width << "x" << m_height
                      << std::endl;
        }
    }
}

void ProcessManager::update_fps_pacing_and_mapping(const std::vector<uint8_t>& frame, int captured_w, int captured_h,
                                                   int cap_min_left, int cap_min_top)
{
    const uint64_t sig = quick_frame_signature(frame, captured_w, captured_h);
    const bool frameChanged = (!m_has_last_sig || sig != m_last_frame_sig);
    m_last_frame_sig = sig;
    m_has_last_sig = true;

    const uint64_t nowMs = rpc_unix_epoch_ms();
    const uint64_t lastInputMs = InputController::last_input_activity_ms();
    const bool recentInput =
        (lastInputMs > 0 && nowMs >= lastInputMs &&
         (nowMs - lastInputMs) <= static_cast<uint64_t>(m_recent_input_boost_ms));

    if (frameChanged || recentInput) {
        m_stable_frame_count = 0;
        m_current_fps = m_active_fps;
    } else {
        ++m_stable_frame_count;
        if (m_stable_frame_count >= m_idle_enter_stable_frames) {
            m_current_fps = m_idle_fps;
        }
    }

    InputController::instance()->set_capture_screen_rect(cap_min_left, cap_min_top, captured_w, captured_h);
    m_last_good_rgb_frame = frame;
    m_last_good_rgb_w = captured_w;
    m_last_good_rgb_h = captured_h;
}

void ProcessManager::finalize_encode_rgb(std::vector<uint8_t>& frame, int captured_w, int captured_h,
                                         std::chrono::steady_clock::time_point t_cap_begin,
                                         std::chrono::steady_clock::time_point t_after_cap, bool applied_layout)
{
    std::vector<uint8_t> h264_data;
    const auto t_enc_begin = std::chrono::steady_clock::now();
    const bool forceKeyframe = applied_layout || m_pending_force_keyframe.load();
    bool ok = encode_rgb(m_av_codec_ctx, frame.data(), captured_w, captured_h, m_encode_frame_seq, h264_data, forceKeyframe);
    const auto t_enc_end = std::chrono::steady_clock::now();

    if (ok && !h264_data.empty()) {
        if (!validate_h264_avcc_payload(h264_data.data(), h264_data.size())) {
            static auto lastInvalidEncLog = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - lastInvalidEncLog > std::chrono::seconds(1)) {
                lastInvalidEncLog = now;
                std::cout << "[encode] invalid h264 avcc payload, drop frame\n";
            }
            if (m_have_last_good_sample) {
                sample = m_last_good_sample;
                request_force_keyframe_with_cooldown();
            } else {
                sample.clear();
            }
            sampleTime_us += get_sample_duration_us();
            return;
        }

        const std::byte* p = reinterpret_cast<const std::byte*>(h264_data.data());
        sample.assign(p, p + h264_data.size());

        bool hasIdr = false, hasSps = false, hasPps = false;
        inspect_h264_avcc_sample(sample, hasIdr, hasSps, hasPps);
        if (hasIdr && (!hasSps || !hasPps) && m_extradata_spspps.has_value()) {
            const auto& spspps = m_extradata_spspps.value();
            rtc::binary patched;
            patched.reserve(spspps.size() + sample.size());
            patched.insert(patched.end(), spspps.begin(), spspps.end());
            patched.insert(patched.end(), sample.begin(), sample.end());
            sample.swap(patched);
        }

        sampleTime_us += get_sample_duration_us();

        m_last_capture_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t_after_cap - t_cap_begin).count());
        m_last_encode_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t_enc_end - t_enc_begin).count());
        m_last_frame_unix_ms = rpc_unix_epoch_ms();
        m_had_successful_video = true;
        m_last_good_sample = sample;
        m_have_last_good_sample = true;
        m_pending_force_keyframe.store(false);
    } else {
        if (m_have_last_good_sample) {
            sample = m_last_good_sample;
            request_force_keyframe_with_cooldown();
        } else {
            sample.clear();
        }
        sampleTime_us += get_sample_duration_us();
    }
}

void ProcessManager::load_next_sample()
{
    if (!running) return;

    static auto last_no_window_diag = std::chrono::steady_clock::now();
    if (!tick_window_and_health(last_no_window_diag)) return;

    int width = 0, height = 0;
    int cap_min_left = 0, cap_min_top = 0;
    const auto t_cap_begin = std::chrono::steady_clock::now();

    const uint64_t cap_now_ms = rpc_unix_epoch_ms();
    const bool use_hw_capture = decide_use_hw_capture(cap_now_ms);

    std::vector<uint8_t> frame;
    if (!grab_rgb_frame(width, height, cap_min_left, cap_min_top, frame, use_hw_capture)) return;

    const auto t_after_cap = std::chrono::steady_clock::now();
    if (discard_if_capture_too_slow(t_cap_begin, t_after_cap, use_hw_capture))
        return;
    if (discard_if_empty_frame(frame, width, height))
        return;
    if (!filter_suspicious_frame(frame, width, height, cap_min_left, cap_min_top))
        return;

    bool applied_layout = false;
    ensure_encoder_layout(width, height, applied_layout);

    update_fps_pacing_and_mapping(frame, width, height, cap_min_left, cap_min_top);
    finalize_encode_rgb(frame, width, height, t_cap_begin, t_after_cap, applied_layout);
}

rtc::binary ProcessManager::get_sample()
{
    return sample;
}

uint64_t ProcessManager::get_sample_time_us()
{
    return sampleTime_us;
}

uint64_t ProcessManager::get_sample_duration_us()
{
    // Keep a stable RTP time axis.
    // Variable pacing (idle fps changes) tends to increase jitter-buffer build-up
    // on the browser receiver, which can manifest as visible flicker/black frames.
    if (m_fps > 0) return 1000000ull / static_cast<uint64_t>(m_fps);
    return 0;
}

std::vector<HWND> ProcessManager::find_all_windows(DWORD pid)
{
    struct EnumData
    {
        DWORD pid;
        std::vector<HWND>* hwnds;
    };

    std::vector<HWND> result;
    EnumData data = { pid, &result };

    EnumWindows(static_cast<WNDENUMPROC>([](HWND hwnd, LPARAM lParam)->BOOL{
        DWORD winPid = 0;
        GetWindowThreadProcessId(hwnd, &winPid);

        auto* data = reinterpret_cast<EnumData*>(lParam);
        // Do not require IsWindowVisible here; some windows become visible shortly after creation.
        // We'll filter for visibility when capturing.
        if (winPid == data->pid) {
            data->hwnds->push_back(hwnd);
        }
        return TRUE;
    }), reinterpret_cast<LPARAM>(&data));

    return result;
}

std::vector<uint8_t> ProcessManager::capture_all_windows_image(DWORD pid, HWND anchorHwnd, int maxBelowMainPx,
                                                               int& outWidth, int& outHeight, int& outMinLeft,
                                                               int& outMinTop)
{
    struct WindowInfo {
        HWND hwnd;
        RECT rect;
        std::vector<uint8_t> image;
    };

    // 1) Enumerate visible windows for this process in z-order.
    std::vector<HWND> hwnds;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        DWORD winPid = 0;
        GetWindowThreadProcessId(hwnd, &winPid);
        if (IsWindowVisible(hwnd) && winPid == (DWORD)lParam) {
            auto* vec = reinterpret_cast<std::vector<HWND>*>(GetPropW(hwnd, L"__zorder_vec"));
            if (vec) vec->push_back(hwnd);
        }
        return TRUE;
        }, (LPARAM)pid);

    // Re-run EnumWindows using static context because callback cannot capture state.
    static std::vector<HWND>* s_zorder_vec = nullptr;
    s_zorder_vec = &hwnds;
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        DWORD winPid = 0;
        GetWindowThreadProcessId(hwnd, &winPid);
        if (IsWindowVisible(hwnd) && winPid == (DWORD)lParam) {
            s_zorder_vec->push_back(hwnd);
        }
        return TRUE;
        }, (LPARAM)pid);
    s_zorder_vec = nullptr;

    if (hwnds.empty()) {
        outWidth = 0;
        outHeight = 0;
        outMinLeft = 0;
        outMinTop = 0;
        return {};
    }

    // 2) Capture image data for each collected window.
    std::vector<WindowInfo> windows;
    for (HWND hwnd : hwnds) {
        RECT clientRc{};
        if (!GetClientRect(hwnd, &clientRc)) continue;
        POINT tl{ clientRc.left, clientRc.top };
        POINT br{ clientRc.right, clientRc.bottom };
        if (!ClientToScreen(hwnd, &tl) || !ClientToScreen(hwnd, &br)) continue;

        const int width = br.x - tl.x;
        const int height = br.y - tl.y;
        if (width <= 0 || height <= 0) continue;

        std::vector<uint8_t> img = m_windowCapture.capture(hwnd, width, height);
        if (img.empty()) continue;

        RECT rect{};
        rect.left = tl.x;
        rect.top = tl.y;
        rect.right = tl.x + width;
        rect.bottom = tl.y + height;
        windows.push_back({ hwnd, rect, std::move(img) });
    }
    if (windows.empty()) {
        outWidth = 0;
        outHeight = 0;
        outMinLeft = 0;
        outMinTop = 0;
        return {};
    }

    // 3) Compute merged bounding rectangle.
    int minLeft = INT_MAX, minTop = INT_MAX, maxRight = INT_MIN, maxBottom = INT_MIN;
    for (const auto& win : windows) {
        minLeft = min(minLeft, win.rect.left);
        minTop = min(minTop, win.rect.top);
        maxRight = max(maxRight, win.rect.right);
        maxBottom = max(maxBottom, win.rect.bottom);
    }
    int totalWidth = maxRight - minLeft;
    int totalHeight = maxBottom - minTop;

    RECT clipRect;
    clipRect.left = minLeft;
    clipRect.top = minTop;
    clipRect.right = maxRight;
    clipRect.bottom = maxBottom;
    if (anchorHwnd && IsWindow(anchorHwnd) && maxBelowMainPx > 0) {
        RECT ar{};
        RECT arClient{};
        if (GetClientRect(anchorHwnd, &arClient)) {
            POINT atl{ arClient.left, arClient.top };
            POINT abr{ arClient.right, arClient.bottom };
            if (ClientToScreen(anchorHwnd, &atl) && ClientToScreen(anchorHwnd, &abr)) {
                ar.left = atl.x;
                ar.top = atl.y;
                ar.right = abr.x;
                ar.bottom = abr.y;
                const LONG capBottom = ar.bottom + static_cast<LONG>(maxBelowMainPx);
                if (clipRect.bottom > capBottom) clipRect.bottom = capBottom;
                if (clipRect.bottom <= clipRect.top) clipRect.bottom = clipRect.top + 2;
            }
        }
    }

    totalWidth = clipRect.right - clipRect.left;
    totalHeight = clipRect.bottom - clipRect.top;
    totalWidth = (totalWidth + 1) & ~1;
    totalHeight = (totalHeight + 1) & ~1;
    outWidth = totalWidth;
    outHeight = totalHeight;
    outMinLeft = clipRect.left;
    outMinTop = clipRect.top;

    // 4) Composite windows back-to-front into the merged clip rectangle.
    std::vector<uint8_t> mergedImage(static_cast<size_t>(totalWidth) * static_cast<size_t>(totalHeight) * 3, 0);
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        const auto& win = *it;
        RECT inter{};
        if (!IntersectRect(&inter, &win.rect, &clipRect)) continue;

        const int w = win.rect.right - win.rect.left;
        const int srcX = inter.left - win.rect.left;
        const int srcY = inter.top - win.rect.top;
        const int dstX = inter.left - clipRect.left;
        const int dstY = inter.top - clipRect.top;
        const int iw = inter.right - inter.left;
        const int ih = inter.bottom - inter.top;
        if (iw <= 0 || ih <= 0 || w <= 0) continue;

        for (int y = 0; y < ih; ++y) {
            const size_t srcRow = (static_cast<size_t>(srcY + y) * static_cast<size_t>(w) + static_cast<size_t>(srcX)) * 3u;
            const size_t dstRow =
                (static_cast<size_t>(dstY + y) * static_cast<size_t>(totalWidth) + static_cast<size_t>(dstX)) * 3u;
            const size_t nbytes = static_cast<size_t>(iw) * 3u;
            if (srcRow + nbytes > win.image.size() || dstRow + nbytes > mergedImage.size()) continue;
            std::memcpy(mergedImage.data() + dstRow, win.image.data() + srcRow, nbytes);
        }
    }

    return mergedImage;
}


HWND ProcessManager::find_main_window(DWORD pid) 
{
    struct HandleData
    {
        DWORD pid;
        HWND hwnd;
    } data = { pid, nullptr };

    auto enumWindowsCallback = [](HWND hwnd, LPARAM lParam) -> BOOL {
        HandleData* pData = reinterpret_cast<HandleData*>(lParam);
        DWORD winPid = 0;
        GetWindowThreadProcessId(hwnd, &winPid);
        if (winPid == pData->pid && GetWindow(hwnd, GW_OWNER) == NULL && IsWindowVisible(hwnd)) 
        {
            pData->hwnd = hwnd;
            return FALSE;
        }
        return TRUE;
        };
    EnumWindows(enumWindowsCallback, reinterpret_cast<LPARAM>(&data));

    return data.hwnd;
}
