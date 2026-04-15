#include "session/window_selection_utils.h"

#include "common/window_ops.h"

#include <algorithm>
#include <cctype>

namespace window_selection_utils {

std::string to_lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string get_window_text_lower(HWND hwnd)
{
    window_ops wops;
    return to_lower_ascii(wops.get_window_text_utf8(hwnd));
}

std::string get_window_class_lower(HWND hwnd)
{
    window_ops wops;
    return to_lower_ascii(wops.get_window_class_utf8(hwnd));
}

std::string exe_stem_lower(const std::string& exe_base_name)
{
    std::string stem = to_lower_ascii(exe_base_name);
    const auto pos = stem.rfind(".exe");
    if (pos != std::string::npos) {
        stem = stem.substr(0, pos);
    }
    return stem;
}

static bool contains_any_token(const std::string& s, const char* const* tokens, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (s.find(tokens[i]) != std::string::npos) return true;
    }
    return false;
}

static bool is_likely_splash_window(HWND hwnd)
{
    const std::string title = get_window_text_lower(hwnd);
    const std::string cls = get_window_class_lower(hwnd);
    static const char* kSplashTokens[] = {
        "splash", "loading", "startup", "welcome", "logo", "screen",
        "启动", "欢迎"
    };
    return contains_any_token(title, kSplashTokens, sizeof(kSplashTokens) / sizeof(kSplashTokens[0])) ||
           contains_any_token(cls, kSplashTokens, sizeof(kSplashTokens) / sizeof(kSplashTokens[0]));
}

int score_window_for_capture(HWND hwnd, DWORD expected_pid, bool require_pid_match)
{
    window_ops wops;
    if (!wops.is_valid(hwnd)) return -1000000;
    const DWORD pid = wops.get_window_pid(hwnd);
    if (!pid) return -1000000;
    if (require_pid_match && pid != expected_pid) return -1000000;

    RECT rc{};
    if (!wops.get_window_rect(hwnd, rc)) return -1000000;
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return -1000000;

    const LONG_PTR style = wops.get_style(hwnd);
    const LONG_PTR ex_style = wops.get_ex_style(hwnd);
    const bool visible = wops.is_visible(hwnd);
    const bool ownerless = wops.is_ownerless_top_level(hwnd);
    const bool tool = ((ex_style & WS_EX_TOOLWINDOW) != 0);
    const bool app_window = ((ex_style & WS_EX_APPWINDOW) != 0);
    const bool caption = ((style & WS_CAPTION) != 0);
    const bool thickframe = ((style & WS_THICKFRAME) != 0);
    const bool minimize_box = ((style & WS_MINIMIZEBOX) != 0);
    const bool maximize_box = ((style & WS_MAXIMIZEBOX) != 0);
    const bool popup_only = ((style & WS_POPUP) != 0) && !caption;

    int score = 0;
    // 可见、可交互、具备主窗口样式的窗体应优先。
    if (visible) score += 40;
    if (ownerless) score += 20;
    if (!tool) score += 20;
    if (app_window) score += 12;
    if (caption) score += 10;
    if (thickframe) score += 8;
    if (minimize_box) score += 6;
    if (maximize_box) score += 6;
    if (popup_only) score -= 16;
    if (tool) score -= 20;
    if (is_likely_splash_window(hwnd)) score -= 80;

    const int area = width * height;
    // 小面积细长窗（启动条/工具条）常导致误采集，显著降分但不做硬拒绝。
    if (area < 40000) {
        score -= 80;
    } else if (area < 120000) {
        score -= 40;
    }
    if (area < 120000 && !thickframe && !minimize_box && !maximize_box) {
        score -= 30;
    }
    score += (std::min)(30, area / 50000);
    return score;
}

} // namespace window_selection_utils

