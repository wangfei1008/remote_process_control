#include "capture/process_surface_enumerator.h"

#include "common/window_rect_utils.h"

#include <cwchar>

namespace {

std::string utf16_to_utf8(const WCHAR* wstr, int len)
{
    if (!wstr || len <= 0) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, wstr, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, out.data(), need, nullptr, nullptr);
    return out;
}

} // namespace

std::vector<ProcessSurfaceInfo> ProcessSurfaceEnumerator::enumerate_visible_top_level(DWORD pid)
{
    std::vector<ProcessSurfaceInfo> out;
    if (pid == 0) return out;

    struct Ctx {
        DWORD target_pid = 0;
        std::vector<ProcessSurfaceInfo>* surfaces = nullptr;
        int next_z = 0;
    } ctx{pid, &out, 0};

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (!c || !c->surfaces) return TRUE;
            DWORD win_pid = 0;
            GetWindowThreadProcessId(hwnd, &win_pid);
            if (!IsWindowVisible(hwnd) || win_pid != c->target_pid) return TRUE;

            // Filter input method window: Check window class name
            char className[256];
            GetClassNameA(hwnd, className, sizeof(className));
            // Input method related class names
            if (strcmp(className, "IME") == 0) return TRUE;          // Default window for input method
            if (strcmp(className, "MSCTFIME UI") == 0) return TRUE;  // TSF Input Method UI

            ProcessSurfaceInfo info;
            info.hwnd = hwnd;
            info.z_order = c->next_z;

            WCHAR title[512]{};
            if (GetWindowTextW(hwnd, title, 512) > 0) {
                info.title = utf16_to_utf8(title, static_cast<int>(wcslen(title)));
            }
            WCHAR cls[256]{};
            if (GetClassNameW(hwnd, cls, 256) > 0) {
                info.class_name = utf16_to_utf8(cls, static_cast<int>(wcslen(cls)));
            }

            RECT rc{};
            if (window_rect_utils::get_effective_window_rect(hwnd, rc)) {
                info.rect_screen = rc;
            }
            ++c->next_z;
            c->surfaces->push_back(std::move(info));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    return out;
}
