#include "session/remote_process_session.h"

#include <algorithm>
#include <iostream>

std::string RemoteProcessSession::basename_from_path(const std::string& path)
{
    const auto pos = path.find_last_of("\\/");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

std::string RemoteProcessSession::get_process_basename(DWORD pid)
{
    HANDLE process_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process_handle) return {};

    char path_buffer[MAX_PATH] = {0};
    DWORD path_size = MAX_PATH;
    std::string process_name;
    if (QueryFullProcessImageNameA(process_handle, 0, path_buffer, &path_size)) {
        process_name = basename_from_path(std::string(path_buffer, path_buffer + path_size));
        std::transform(process_name.begin(), process_name.end(), process_name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    CloseHandle(process_handle);
    return process_name;
}

bool RemoteProcessSession::launch_process(const std::string& exe_path,
                                          PROCESS_INFORMATION& out_process_info,
                                          DWORD& out_launch_pid,
                                          DWORD& out_capture_pid,
                                          HWND& out_main_window,
                                          std::string& out_target_exe_base_name)
{
    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(startup_info);

    out_target_exe_base_name = basename_from_path(exe_path);
    std::transform(out_target_exe_base_name.begin(), out_target_exe_base_name.end(), out_target_exe_base_name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (!CreateProcessA(exe_path.c_str(), NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startup_info, &out_process_info)) {
        std::cout << "[proc] CreateProcess failed, exe=" << exe_path
                  << " error=" << GetLastError() << std::endl;
        return false;
    }

    std::cout << "[proc] CreateProcess ok, pid=" << out_process_info.dwProcessId
              << " exe=" << exe_path << std::endl;

    out_launch_pid = out_process_info.dwProcessId;
    out_capture_pid = out_launch_pid;

    // 不要因等待窗口而阻塞流创建。
    out_main_window = find_main_window(out_process_info.dwProcessId);
    if (!out_main_window) {
        auto windows = find_all_windows(out_process_info.dwProcessId);
        if (!windows.empty()) out_main_window = windows.front();
    }
    return true;
}

void RemoteProcessSession::terminate_processes(PROCESS_INFORMATION& process_info, DWORD capture_pid, DWORD launch_pid)
{
    if (process_info.hProcess) {
        TerminateProcess(process_info.hProcess, 0);
        CloseHandle(process_info.hProcess);
        process_info.hProcess = nullptr;
    }
    if (process_info.hThread) {
        CloseHandle(process_info.hThread);
        process_info.hThread = nullptr;
    }

    if (capture_pid != 0 && capture_pid != launch_pid) {
        HANDLE process_handle = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, capture_pid);
        if (process_handle) {
            TerminateProcess(process_handle, 0);
            CloseHandle(process_handle);
        }
    }
}

std::vector<HWND> RemoteProcessSession::find_all_windows(DWORD pid) const
{
    struct Enum_data {
        DWORD pid = 0;
        std::vector<HWND>* hwnds = nullptr;
    } enum_data{pid, nullptr};

    std::vector<HWND> result;
    enum_data.hwnds = &result;

    EnumWindows(static_cast<WNDENUMPROC>([](HWND hwnd, LPARAM l_param) -> BOOL {
        auto* data = reinterpret_cast<Enum_data*>(l_param);
        DWORD window_pid = 0;
        GetWindowThreadProcessId(hwnd, &window_pid);
        if (window_pid == data->pid) {
            data->hwnds->push_back(hwnd);
        }
        return TRUE;
    }), reinterpret_cast<LPARAM>(&enum_data));

    return result;
}

HWND RemoteProcessSession::find_main_window(DWORD pid) const
{
    struct Handle_data {
        DWORD pid = 0;
        HWND hwnd = nullptr;
    } handle_data{pid, nullptr};

    EnumWindows([](HWND hwnd, LPARAM l_param) -> BOOL {
        auto* data = reinterpret_cast<Handle_data*>(l_param);
        DWORD window_pid = 0;
        GetWindowThreadProcessId(hwnd, &window_pid);
        if (window_pid == data->pid && GetWindow(hwnd, GW_OWNER) == NULL && IsWindowVisible(hwnd)) {
            data->hwnd = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&handle_data));

    return handle_data.hwnd;
}

HWND RemoteProcessSession::find_window_by_exe_basename(const std::string& exe_base_name) const
{
    if (exe_base_name.empty()) return nullptr;

    std::string target_name = exe_base_name;
    std::transform(target_name.begin(), target_name.end(), target_name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    struct Best_window {
        HWND hwnd = nullptr;
        int area = 0;
    } best_window;

    struct Enum_ctx {
        const char* target = nullptr;
        Best_window* best = nullptr;
    } enum_ctx{target_name.c_str(), &best_window};

    EnumWindows([](HWND hwnd, LPARAM l_param) -> BOOL {
        auto* ctx = reinterpret_cast<Enum_ctx*>(l_param);
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

        RECT rect{};
        if (!GetWindowRect(hwnd, &rect)) return TRUE;
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return TRUE;

        std::string base_name = RemoteProcessSession::get_process_basename(pid);
        if (base_name.empty() || base_name != ctx->target) return TRUE;

        const int area = width * height;
        if (area > ctx->best->area) {
            ctx->best->area = area;
            ctx->best->hwnd = hwnd;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&enum_ctx));

    return best_window.hwnd;
}

