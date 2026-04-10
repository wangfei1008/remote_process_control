#pragma once

#include <cstdint>

class CaptureBackendState {
public:
    void configure(bool session_uses_dxgi, uint32_t capture_degrade_ms, bool allow_auto_dxgi_disable);
    void reset_for_stream_start();

    void on_dxgi_empty(uint64_t now_ms, bool& should_reset_duplication);
    void on_dxgi_success();
    void on_slow_capture(uint64_t now_ms);
    void on_top_black_strip_detected(uint64_t now_ms, bool hw_capture_active);
    void on_no_top_black_strip();

    bool is_dxgi_disabled_for_session() const { return m_disable_dxgi_for_session; }
    int get_top_black_strip_streak() const { return m_top_black_strip_streak; }
    int get_dxgi_instability_score() const { return m_dxgi_instability_score; }
    bool get_last_capture_used_hw() const { return m_last_capture_used_hw; }

    void set_last_capture_used_hw(bool v) { m_last_capture_used_hw = v; }

private:
    bool m_session_uses_dxgi = false;
    uint32_t m_capture_degrade_ms = 2500;
    bool m_allow_auto_dxgi_disable = true;

    int m_top_black_strip_streak = 0;
    int m_top_black_strip_disable_dxgi_threshold = 3;
    bool m_disable_dxgi_for_session = false;

    bool m_last_capture_used_hw = false;
    int m_dxgi_instability_score = 0;
    int m_dxgi_disable_score_threshold = 5;

    int m_dxgi_fail_streak = 0;
    int m_dxgi_fail_reset_threshold = 6;
};
