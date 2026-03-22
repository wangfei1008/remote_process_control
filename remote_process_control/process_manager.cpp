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

static uint64_t rpc_unix_epoch_ms()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
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
    , running(false)
    , sampleTime_us(0)
{
    ZeroMemory(&m_pi, sizeof(m_pi));
    
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


        // ?????????YUV420???
        m_width = (m_width + 1) & ~1;
		m_height = (m_height + 1) & ~1;// ?????????
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

void ProcessManager::load_next_sample()
{
    if (!running)
        return;

    if (m_had_successful_video && !m_exit_notified) {
        // 必须监视「当前采集所属 PID」（m_capturePid）。Win11 等下记事本常为 stub 启动后退出，
        // 真实窗口在另一进程（rebound 后 m_capturePid != m_launchPid）；若仍查 m_pi.hProcess
        // 会在 stub 退出后误报「远端已结束」并停流。
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
            // 句柄失效常见于：启动器窗口关闭后由新 HWND 接管、UWP/WinUI 重建主窗等。
            // 若此处直接 notify，前端会立刻关窗；改为清空后走下方重新枚举窗口。
            m_mainWindow = nullptr;
        }
    }

    static auto lastDiag = std::chrono::steady_clock::now();

    if (!m_mainWindow) {
        m_mainWindow = find_main_window(m_capturePid);
        if (!m_mainWindow) {
            auto wins = find_all_windows(m_capturePid);
            // Prefer a visible window; otherwise keep the first one as candidate.
            for (auto h : wins) {
                if (IsWindowVisible(h)) {
                    m_mainWindow = h;
                    break;
                }
            }
            if (!m_mainWindow && !wins.empty()) m_mainWindow = wins.front();
        }
        // If the original PID has no windows (common for launcher/stub processes),
        // try to find a top-level visible window by executable base name.
        if (!m_mainWindow && !m_targetExeBaseName.empty()) {
            HWND h = find_window_by_exe_basename(m_targetExeBaseName);
            if (h) {
                DWORD realPid = 0;
                GetWindowThreadProcessId(h, &realPid);
                if (realPid) {
                    std::cout << "[proc] rebound window by exe, oldPid=" << m_pi.dwProcessId
                              << " newPid=" << realPid << " hwnd=" << h << std::endl;
                    m_capturePid = realPid;
                    m_mainWindow = h;
                }
            }
        }
        if (!m_mainWindow) {
            // Keep advancing timestamps even if the window isn't ready yet,
            // otherwise the stream scheduler may stall on video forever.
            auto now = std::chrono::steady_clock::now();
            if (now - lastDiag > std::chrono::seconds(1)) {
                lastDiag = now;
                auto wins = find_all_windows(m_capturePid);
                std::cout << "[proc] no window yet, pid=" << m_capturePid
                          << " windows=" << wins.size() << std::endl;
                int shown = 0;
                for (auto h : wins) {
                    if (shown++ >= 5) break;
                    char title[256] = {0};
                    GetWindowTextA(h, title, sizeof(title) - 1);
                    std::cout << "  hwnd=" << h
                              << " visible=" << (IsWindowVisible(h) ? 1 : 0)
                              << " title=" << title << std::endl;
                }
            }
            sample.clear();
            sampleTime_us += get_sample_duration_us();
            return;
        }
    }

    // ???????????????std::vector<uint8_t>
    //std::vector<uint8_t> frame = m_windowCapture.capture(m_mainWindow, m_width, m_height);
    int width = 0, height = 0;
    int capMinLeft = 0, capMinTop = 0;
    const auto t_cap_begin = std::chrono::steady_clock::now();
    std::vector<uint8_t> frame = capture_all_windows_image(
        m_capturePid, m_mainWindow, 1024, width, height, capMinLeft, capMinTop);
    const auto t_after_cap = std::chrono::steady_clock::now();
    if (frame.empty() || width <= 0 || height <= 0) {
        sample.clear();
        sampleTime_us += get_sample_duration_us();
        return;
    }
    const bool layout_changed = (width != m_width || height != m_height);
    if (layout_changed) {
        // ?????????
        if (m_av_codec_ctx) {
            destroy_h264_encoder(m_av_codec_ctx); // ?????????????????
            m_av_codec_ctx = nullptr;
        }
        m_encode_frame_seq = 0;
        // ?????????
        m_width = width;
        m_height = height;
        // ????????????
        m_av_codec_ctx = create_h264_encoder(m_width, m_height, m_fps);
        m_extradata_spspps = parse_avcc_spspps(m_av_codec_ctx);
    }

    InputController::instance()->set_capture_screen_rect(capMinLeft, capMinTop, m_width, m_height);

    // ?????H264
    std::vector<uint8_t> h264_data;
    const auto t_enc_begin = std::chrono::steady_clock::now();
    bool ok = encode_rgb(m_av_codec_ctx, frame.data(), m_width, m_height, m_encode_frame_seq, h264_data,
                         layout_changed);
    const auto t_enc_end = std::chrono::steady_clock::now();
    if (ok && !h264_data.empty()) {
        std::vector<std::byte> h264_bytes(h264_data.size());
        std::memcpy(h264_bytes.data(), h264_data.data(), h264_data.size());
        sample = rtc::binary(h264_bytes.begin(), h264_bytes.end());
        sampleTime_us += get_sample_duration_us();

        m_last_capture_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t_after_cap - t_cap_begin).count());
        m_last_encode_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t_enc_end - t_enc_begin).count());
        m_last_frame_unix_ms = rpc_unix_epoch_ms();
        m_had_successful_video = true;

        bool has7 = false, has8 = false, has5 = false;
        size_t i = 0;
        while (i + 4 <= sample.size()) {
            uint32_t length;
            std::memcpy(&length, sample.data() + i, sizeof(uint32_t));
            length = ntohl(length);
            auto naluStartIndex = i + 4;
            auto naluEndIndex = naluStartIndex + length;
            if (naluEndIndex > sample.size()) break; // ??????
            auto header = reinterpret_cast<rtc::NalUnitHeader*>(sample.data() + naluStartIndex);
            auto type = header->unitType();
            switch (type) {
            case 7:
                has7 = true;
                previousUnitType7 = { sample.begin() + i, sample.begin() + naluEndIndex };
                break;
            case 8:
                has8 = true;
                previousUnitType8 = { sample.begin() + i, sample.begin() + naluEndIndex };
                break;
            case 5:
                has5 = true;
                previousUnitType5 = { sample.begin() + i, sample.begin() + naluEndIndex };
                break;
            }
            i = naluEndIndex;
        }

        // Browser decoders often need SPS/PPS before the first IDR.
        // If we got an IDR but this access unit doesn't contain SPS/PPS, prepend cached SPS/PPS.
        if (has5 && (!has7 || !has8)) {
            std::optional<std::vector<std::byte>> prefix = std::nullopt;
            if (previousUnitType7.has_value() && previousUnitType8.has_value()) {
                std::vector<std::byte> v;
                v.reserve(previousUnitType7->size() + previousUnitType8->size());
                v.insert(v.end(), previousUnitType7->begin(), previousUnitType7->end());
                v.insert(v.end(), previousUnitType8->begin(), previousUnitType8->end());
                prefix = std::move(v);
            } else if (m_extradata_spspps.has_value()) {
                prefix = m_extradata_spspps;
            }

            if (prefix.has_value()) {
                rtc::binary merged;
                merged.reserve(prefix->size() + sample.size());
                merged.insert(merged.end(), prefix->begin(), prefix->end());
                merged.insert(merged.end(), sample.begin(), sample.end());
                sample = std::move(merged);
            }
        }
    } else {
        // 编码失败或本帧无码流时也必须推进时间轴并清空 sample，否则会反复 sendFrame
        // 上一帧数据 + 相同 sampleTime，浏览器侧表现为画面冻结、只显示局部/首帧。
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
    if (m_fps > 0)
        return 1000000ull / m_fps;
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

    // 1. ???Z??????????????
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

    // ????EnumWindows?????????vector?????????????????????
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

    // 2. ???????????????
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

    // 3. ??????????????
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

    // 4. Z???????????? clipRect ???????????????????????
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
