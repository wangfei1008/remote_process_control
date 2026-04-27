#pragma once

// ============================================================
// window_score_policy.h
// 窗口评分策略：决定哪个窗口更适合作为采集目标
// ============================================================

#include "../infra/win32_window.h"
#include <optional>
#include <span>
#include <string_view>

namespace capture {

class WindowScorePolicy {
public:
    struct Context {
        DWORD expected_pid      = 0;
        bool  require_pid_match = false;
    };

    virtual ~WindowScorePolicy() = default;

    // 主入口：nullopt = 排除；int = 候选分（越高越优先）
    virtual std::optional<int> score(const win32::WindowInfo& info, const Context& ctx) const;

protected:
    virtual int style_score(const win32::WindowInfo& info) const;
    virtual int area_score(int area) const;
    virtual int splash_penalty(const win32::WindowInfo& info) const;
    virtual std::span<const std::string_view> splash_tokens() const;

private:
    static bool contains_any(const std::string& s,std::span<const std::string_view> tokens);
};

} // namespace capture
