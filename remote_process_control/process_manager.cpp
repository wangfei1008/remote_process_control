#include "process_manager.h"
#include "input_controller.h"
#include <tlhelp32.h>
#include <cstdint>
#include <vector>
#include <thread>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

static uint64_t rpc_unix_epoch_ms()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

static bool rpc_probe_dxgi_capture_support()
{
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    const bool ok = (dxgi != nullptr && d3d11 != nullptr);
    if (dxgi) FreeLibrary(dxgi);
    if (d3d11) FreeLibrary(d3d11);
    return ok;
}

// Parse H264 avcC extradata into length-prefixed SPS/PPS NAL units.
// Output format: [uint32_be length][nalu bytes]...
static std::optional<std::vector<std::byte>> parse_avcc_spspps(const AVCodecContext* ctx)
{
    if (!ctx || !ctx->extradata || ctx->extradata_size <= 0) return std::nullopt;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(ctx->extradata);
    const size_t size = static_cast<size_t>(ctx->extradata_size);
    if (size < 7) return std::nullopt;
    if (p[0] != 1) return std::nullopt; // configurationVersion

    size_t off = 5;
    if (off >= size) return std::nullopt;

    const uint8_t numSps = p[off] & 0x1F;
    off += 1;

    std::vector<std::byte> out;
    auto append_nalu = [&](const uint8_t* nalu, size_t naluSize) {
        if (!nalu || naluSize == 0) return;
        const uint32_t len = static_cast<uint32_t>(naluSize);
        const uint32_t len_be =
            (len & 0x000000FFu) << 24 |
            (len & 0x0000FF00u) << 8 |
            (len & 0x00FF0000u) >> 8 |
            (len & 0xFF000000u) >> 24;
        const std::byte* lenBytes = reinterpret_cast<const std::byte*>(&len_be);
        out.insert(out.end(), lenBytes, lenBytes + 4);
        const std::byte* dataBytes = reinterpret_cast<const std::byte*>(nalu);
        out.insert(out.end(), dataBytes, dataBytes + naluSize);
    };

    for (uint8_t i = 0; i < numSps; ++i) {
        if (off + 2 > size) return std::nullopt;
        const uint16_t spsLen = (uint16_t(p[off]) << 8) | uint16_t(p[off + 1]);
        off += 2;
        if (off + spsLen > size) return std::nullopt;
        append_nalu(p + off, spsLen);
        off += spsLen;
    }

    if (off + 1 > size) return std::nullopt;
    const uint8_t numPps = p[off];
    off += 1;

    for (uint8_t i = 0; i < numPps; ++i) {
        if (off + 2 > size) return std::nullopt;
        const uint16_t ppsLen = (uint16_t(p[off]) << 8) | uint16_t(p[off + 1]);
        off += 2;
        if (off + ppsLen > size) return std::nullopt;
        append_nalu(p + off, ppsLen);
        off += ppsLen;
    }

    if (out.empty()) return std::nullopt;
    return out;
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
        m_idle_fps = std::max(1, std::atoi(envIdleFps));
    }
    if (const char* envActiveFps = std::getenv("RPC_ACTIVE_FPS")) {
        m_active_fps = std::max(1, std::atoi(envActiveFps));
    }
    if (m_idle_fps > m_active_fps) m_idle_fps = m_active_fps;
    m_fps = m_active_fps;
    m_current_fps = m_active_fps;

    m_hw_capture_supported = (rpc_probe_dxgi_capture_support() && m_dxgiCapture.is_available());
    if (const char* envCaptureBackend = std::getenv("RPC_CAPTURE_BACKEND")) {
        std::string mode = envCaptureBackend;
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mode == "dxgi") m_hw_capture_requested = true;
        else if (mode == "gdi") m_hw_capture_requested = false;
        else m_hw_capture_requested = m_hw_capture_supported; // auto
    } else {
        m_hw_capture_requested = m_hw_capture_supported; // default auto
    }
    m_hw_capture_active = (m_hw_capture_requested && m_hw_capture_supported);
    std::cout << "[capture] dxgi_supported=" << (m_hw_capture_supported ? 1 : 0)
              << " requested=" << (m_hw_capture_requested ? 1 : 0)
              << " active=" << (m_hw_capture_active ? 1 : 0)
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

    EnumWindows([](HWND hwnd, LPARAM lParam)->BOOL {
        auto* best = reinterpret_cast<Best*>(lParam);
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return TRUE;

        char title[2] = {0};
        GetWindowTextA(hwnd, title, 1); // only check non-empty quickly
        // allow empty title too; don't filter here

        std::string base = ProcessManager::get_process_basename(pid);
        if (base.empty()) return TRUE;

        // compare
        // NOTE: we cannot capture 'target' inside callback, so we'll store it in window property
        return TRUE;
    }, reinterpret_cast<LPARAM>(&best));

    // The above EnumWindows cannot capture 'target' (lambda is converted to function pointer).
    // Use a static to pass target for this call (single-thread usage is fine here).
    struct TargetCtx { const char* target; Best* best; };
    static TargetCtx* s_ctx = nullptr;
    TargetCtx ctx{ target.c_str(), &best };
    s_ctx = &ctx;
    EnumWindows([](HWND hwnd, LPARAM)->BOOL {
        if (!s_ctx) return FALSE;
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

        if (base != s_ctx->target) return TRUE;

        const int area = w * h;
        if (area > s_ctx->best->area) {
            s_ctx->best->area = area;
            s_ctx->best->hwnd = hwnd;
        }
        return TRUE;
    }, 0);
    s_ctx = nullptr;

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
    RECT rect;
    if (GetWindowRect(m_mainWindow, &rect))
    {
  //      POINT pt = { rect.left, rect.top };
  //      ClientToScreen(m_mainWindow, &pt);
		//rect.left = pt.x;
		//rect.top = pt.y;
  //      pt = { rect.right, rect.bottom };
  //      ClientToScreen(m_mainWindow, &pt);
		//rect.right = pt.x;
		//rect.bottom = pt.y;
		//rect.bottom = pt.y;
        m_width = rect.right - rect.left;
        m_height = rect.bottom - rect.top;


        // Enforce even dimensions for YUV420 encoding.
        m_width = (m_width + 1) & ~1;
		m_height = (m_height + 1) & ~1; // keep height even
  //      m_width = (m_width + 3) & ~3;
  //      m_height = (m_height + 3) & ~3;
  //      m_width = 820;
  //      m_height = 620;
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
    sampleTime_us = 0;
    m_encode_frame_seq = 0;
    m_stable_frame_count = 0;
    m_has_last_sig = false;
    m_last_frame_sig = 0;
    m_current_fps = m_active_fps;
}

void ProcessManager::stop()
{
    running = false;
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
    const int sampleRows = std::min(height, 32);
    const int sampleCols = std::min(width, 64);
    const int rowStep = std::max(1, height / sampleRows);
    const int colStep = std::max(1, width / sampleCols);
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
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return {};
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return {};
    auto frame = m_windowCapture.capture(hwnd, width, height);
    if (frame.empty()) return {};
    outWidth = width & ~1;
    outHeight = height & ~1;
    outMinLeft = rect.left;
    outMinTop = rect.top;
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

void ProcessManager::load_next_sample()
{
    if (!running)
        return;

    if (m_had_successful_video && !m_exit_notified) {
        if (m_capturePid != 0) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, m_capturePid);
            if (hProc) {
                DWORD code = STILL_ACTIVE;
                if (GetExitCodeProcess(hProc, &code) && code != STILL_ACTIVE) {
                    CloseHandle(hProc);
                    notify_remote_exit();
                    return;
                }
                CloseHandle(hProc);
            }
        }
        if (m_mainWindow && !IsWindow(m_mainWindow)) {
            m_mainWindow = nullptr;
        }
    }

    static auto lastDiag = std::chrono::steady_clock::now();
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

        if (!m_mainWindow && !m_targetExeBaseName.empty()) {
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
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            if (now - lastDiag > std::chrono::seconds(1)) {
                lastDiag = now;
                std::vector<HWND> wins = find_all_windows(m_capturePid);
                std::cout << "[proc] no window yet, pid=" << m_capturePid
                          << " windows=" << wins.size() << std::endl;
            }
            sample.clear();
            sampleTime_us += get_sample_duration_us();
            return;
        }
    }

    int width = 0, height = 0;
    int capMinLeft = 0, capMinTop = 0;
    const std::chrono::steady_clock::time_point t_cap_begin = std::chrono::steady_clock::now();

    std::vector<uint8_t> frame;
    if (m_hw_capture_active) {
        if (m_capture_all_windows) {
            // Current DXGI path is optimized for single-window low-latency capture.
            // Keep multi-window mode on existing compositor path.
            frame = capture_all_windows_image(m_capturePid, m_mainWindow, 1024, width, height, capMinLeft, capMinTop);
        } else {
            frame = m_dxgiCapture.capture_window_rgb(m_mainWindow, width, height, capMinLeft, capMinTop);
            if (frame.empty()) {
                // Runtime fallback if desktop duplication temporarily fails.
                frame = capture_main_window_image(m_mainWindow, width, height, capMinLeft, capMinTop);
            }
        }
    } else if (m_capture_all_windows) {
        frame = capture_all_windows_image(m_capturePid, m_mainWindow, 1024, width, height, capMinLeft, capMinTop);
    } else {
        frame = capture_main_window_image(m_mainWindow, width, height, capMinLeft, capMinTop);
    }

    const std::chrono::steady_clock::time_point t_after_cap = std::chrono::steady_clock::now();
    if (frame.empty() || width <= 0 || height <= 0) {
        sample.clear();
        sampleTime_us += get_sample_duration_us();
        return;
    }

    const uint64_t sig = quick_frame_signature(frame, width, height);
    const bool frameChanged = (!m_has_last_sig || sig != m_last_frame_sig);
    m_last_frame_sig = sig;
    m_has_last_sig = true;

    const uint64_t nowMs = rpc_unix_epoch_ms();
    const uint64_t lastInputMs = InputController::last_input_activity_ms();
    const bool recentInput = (lastInputMs > 0 && nowMs >= lastInputMs && (nowMs - lastInputMs) <= static_cast<uint64_t>(m_recent_input_boost_ms));

    if (frameChanged || recentInput) {
        m_stable_frame_count = 0;
        m_current_fps = m_active_fps;
    } else {
        ++m_stable_frame_count;
        if (m_stable_frame_count >= m_idle_enter_stable_frames) {
            m_current_fps = m_idle_fps;
        }
    }

    const bool layout_changed = (width != m_width || height != m_height);
    if (layout_changed) {
        if (m_av_codec_ctx) {
            destroy_h264_encoder(m_av_codec_ctx);
            m_av_codec_ctx = nullptr;
        }
        m_encode_frame_seq = 0;
        m_width = width;
        m_height = height;
        m_av_codec_ctx = create_h264_encoder(m_width, m_height, m_fps);
        m_extradata_spspps = parse_avcc_spspps(m_av_codec_ctx);
    }

    InputController::instance()->set_capture_screen_rect(capMinLeft, capMinTop, m_width, m_height);

    std::vector<uint8_t> h264_data;
    const std::chrono::steady_clock::time_point t_enc_begin = std::chrono::steady_clock::now();
    bool ok = encode_rgb(m_av_codec_ctx, frame.data(), m_width, m_height, m_encode_frame_seq, h264_data,
                         layout_changed);
    const std::chrono::steady_clock::time_point t_enc_end = std::chrono::steady_clock::now();

    if (ok && !h264_data.empty()) {
        // Send the encoded H264 access unit directly as the client payload.
        sample = rtc::binary(h264_data.begin(), h264_data.end());
        sampleTime_us += get_sample_duration_us();

        m_last_capture_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t_after_cap - t_cap_begin).count());
        m_last_encode_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t_enc_end - t_enc_begin).count());
        m_last_frame_unix_ms = rpc_unix_epoch_ms();
        m_had_successful_video = true;
    } else {
        // Even on encoding failure or empty output, advance timeline and clear sample to avoid repeated sendFrame.
        sample.clear();
        sampleTime_us += get_sample_duration_us();
    }
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
    if (m_current_fps > 0)
        return 1000000ull / static_cast<uint64_t>(m_current_fps);
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

    EnumWindows(static_cast<WNDENUMPROC>([](HWND hwnd, LPARAM lParam)->BOOL __stdcall {
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
        RECT rect;
        if (GetWindowRect(hwnd, &rect)) {
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;
            if (width > 0 && height > 0) {
                std::vector<uint8_t> img = m_windowCapture.capture(hwnd, width, height);
                if (!img.empty())
                    windows.push_back({ hwnd, rect, std::move(img) });
            }
        }
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
        if (GetWindowRect(anchorHwnd, &ar)) {
            const LONG capBottom = ar.bottom + static_cast<LONG>(maxBelowMainPx);
            if (clipRect.bottom > capBottom) {
                clipRect.bottom = capBottom;
            }
            if (clipRect.bottom <= clipRect.top) {
                clipRect.bottom = clipRect.top + 2;
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
