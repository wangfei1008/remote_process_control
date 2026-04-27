// ============================================================
// win32_process.cpp
// ============================================================

#include "win32_process.h"
#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#  include <TlHelp32.h>
#  include <psapi.h>

namespace win32 {

// ---- 内部辅助 -----------------------------------------------
namespace {
constexpr DWORD kQueryAccess = PROCESS_QUERY_LIMITED_INFORMATION;
} // namespace

// ---- 句柄操作 -----------------------------------------------

ScopedHandle Process::open(DWORD pid, DWORD access) const {
    if (pid == 0) return ScopedHandle{};
    return ScopedHandle{ OpenProcess(access, FALSE, pid) };
}

// ---- 查询 ---------------------------------------------------

std::optional<std::string> Process::query_image_path(HANDLE h) const {
    if (!h) return std::nullopt;
    char buf[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameA(h, 0, buf, &size)) return std::nullopt;
    return std::string(buf, buf + size);
}

std::optional<std::string> Process::query_image_path(DWORD pid) const {
    auto h = open(pid, kQueryAccess);
    if (!h) return std::nullopt;
    return query_image_path(h.get());
}

std::string Process::basename_lower(DWORD pid) const {
    auto path = query_image_path(pid);
    if (!path) return {};
    return to_lower_ascii(basename_from_path(*path));
}

bool Process::is_running(HANDLE h) const {
    if (!h) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) return false;
    return code == STILL_ACTIVE;
}

bool Process::is_running(DWORD pid) const {
    if (pid == 0) return false;
    auto h = open(pid, kQueryAccess);
    if (!h) return false;
    return is_running(h.get());
}

DWORD Process::exit_code(HANDLE h) const {
    if (!h) return 0;
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    return code;
}

// ---- 终止 ---------------------------------------------------

bool Process::terminate(HANDLE h, UINT code) const {
    if (!h) return false;
    return TerminateProcess(h, code) != FALSE;
}

bool Process::terminate(DWORD pid, UINT code) const {
    if (pid == 0) return false;
    auto h = open(pid, PROCESS_TERMINATE | kQueryAccess);
    if (!h) return false;
    return terminate(h.get(), code);
}

// ---- Toolhelp -----------------------------------------------

DWORD Process::parent_pid(DWORD pid) const {
    if (pid == 0) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    ScopedHandle guard{ snap };

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) return pe.th32ParentProcessID;
        } while (Process32NextW(snap, &pe));
    }
    return 0;
}

bool Process::is_descendant_of(DWORD pid, DWORD ancestor, int max_depth) const {
    if (pid == 0 || ancestor == 0) return false;
    if (pid == ancestor) return true;
    DWORD cur = pid;
    for (int d = 0; d < max_depth; ++d) {
        cur = parent_pid(cur);
        if (cur == 0)        return false;
        if (cur == ancestor) return true;
    }
    return false;
}

// ---- 工具 ---------------------------------------------------

/*static*/
std::string Process::basename_from_path(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

/*static*/
std::string Process::to_lower_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace win32

#else // !_WIN32 ------------------------------------------------

namespace win32 {

ScopedHandle Process::open(DWORD, DWORD) const             { return ScopedHandle{}; }
std::optional<std::string> Process::query_image_path(HANDLE) const { return std::nullopt; }
std::optional<std::string> Process::query_image_path(DWORD)  const { return std::nullopt; }
std::string  Process::basename_lower(DWORD) const          { return {}; }
bool         Process::is_running(HANDLE) const             { return false; }
bool         Process::is_running(DWORD)  const             { return false; }
DWORD        Process::exit_code(HANDLE)  const             { return 0; }
bool         Process::terminate(HANDLE, UINT) const        { return false; }
bool         Process::terminate(DWORD,  UINT) const        { return false; }
DWORD        Process::parent_pid(DWORD)       const        { return 0; }
bool         Process::is_descendant_of(DWORD, DWORD, int) const { return false; }

/*static*/ std::string Process::basename_from_path(const std::string& path) { return path; }
/*static*/ std::string Process::to_lower_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace win32

#endif