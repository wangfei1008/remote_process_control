// ============================================================
// window_score_policy.cpp
// ============================================================

#include "window_score_policy.h"
#include <algorithm>
#include <array>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace capture {

// ---- splash 关键字 ------------------------------------------
static constexpr std::array<std::string_view, 8> kSplashTokens = {
    "splash", "loading", "startup", "welcome",
    "logo",   "screen",  "\xe5\x90\xaf\xe5\x8a\xa8", // "启动" UTF-8
    "\xe6\xac\xa2\xe8\xbf\x8e"                        // "欢迎" UTF-8
};

/*virtual*/
std::span<const std::string_view> WindowScorePolicy::splash_tokens() const {
    return { kSplashTokens.data(), kSplashTokens.size() };
}

// ---- 内部工具 -----------------------------------------------
/*static*/
bool WindowScorePolicy::contains_any(const std::string& s, std::span<const std::string_view> tokens) {
    for (auto& tok : tokens)
        if (s.find(tok) != std::string::npos) return true;
    return false;
}

// ---- 主评分入口 ----------------------------------------------
/*virtual*/
std::optional<int> WindowScorePolicy::score(const win32::WindowInfo& info, const Context& ctx) const {
    // 无效窗口直接排除
    if (!info.valid) return std::nullopt;
    if (ctx.require_pid_match && info.pid != ctx.expected_pid)
        return std::nullopt;

    // 无有效 rect 的窗口排除
    if (!info.rect_ok) return std::nullopt;
    const int w = info.rect_screen.right  - info.rect_screen.left;
    const int h = info.rect_screen.bottom - info.rect_screen.top;
    if (w <= 0 || h <= 0) return std::nullopt;

    const int area = w * h;
    int s = style_score(info) + area_score(area) + splash_penalty(info);
    return s;
}

// ---- style 分 -----------------------------------------------
/*virtual*/
int WindowScorePolicy::style_score(const win32::WindowInfo& info) const {
#if defined(_WIN32)
    const bool tool        = (info.ex_style & WS_EX_TOOLWINDOW) != 0;
    const bool app_window  = (info.ex_style & WS_EX_APPWINDOW)  != 0;
    const bool caption     = (info.style    & WS_CAPTION)        != 0;
    const bool thickframe  = (info.style    & WS_THICKFRAME)     != 0;
    const bool minimize_box= (info.style    & WS_MINIMIZEBOX)    != 0;
    const bool maximize_box= (info.style    & WS_MAXIMIZEBOX)    != 0;
    const bool popup_only  = (info.style    & WS_POPUP) != 0 && !caption;

    int s = 0;
    if (info.visible)   s += 40;
    if (info.ownerless) s += 20;
    if (!tool)          s += 20;
    if (app_window)     s += 12;
    if (caption)        s += 10;
    if (thickframe)     s +=  8;
    if (minimize_box)   s +=  6;
    if (maximize_box)   s +=  6;
    if (popup_only)     s -= 16;
    if (tool)           s -= 20;
    return s;
#else
    return info.visible ? 40 : 0;
#endif
}

// ---- 面积分 -------------------------------------------------
/*virtual*/
int WindowScorePolicy::area_score(int area) const {
    int s = 0;
    if      (area < 40000)  s -= 80;
    else if (area < 120000) s -= 40;
    s += min(30, area / 50000);
    return s;
}

// ---- splash 惩罚 --------------------------------------------
/*virtual*/
int WindowScorePolicy::splash_penalty(const win32::WindowInfo& info) const {
    const auto tokens = splash_tokens();
    if (contains_any(info.title,      tokens)) return -80;
    if (contains_any(info.class_name, tokens)) return -80;
    return 0;
}

} // namespace capture
