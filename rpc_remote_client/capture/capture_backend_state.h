#pragma once

#include <atomic>
#include <cstdint>

class CaptureBackendState {
public:
    void configure(bool hw_capture_supported,
                   bool hw_capture_active,
                   bool lock_capture_backend,
                   bool locked_use_hw_capture,
                   uint32_t capture_degrade_ms);
    void reset_for_stream_start();

    bool decide_use_hw_capture(uint64_t cap_now_ms);

    void on_dxgi_empty(uint64_t now_ms, bool& should_reset_duplication);
    void on_dxgi_success();
    void on_slow_capture(uint64_t now_ms, bool use_hw_capture);
    void on_top_black_strip_detected(uint64_t now_ms, bool hw_capture_active);
    void on_no_top_black_strip();

    bool is_dxgi_disabled_for_session() const { return m_disable_dxgi_for_session; }
    int get_top_black_strip_streak() const { return m_top_black_strip_streak; }
    int get_dxgi_instability_score() const { return m_dxgi_instability_score; }
    uint64_t get_force_software_capture_until_unix_ms() const {
        return m_force_software_capture_until_unix_ms.load(std::memory_order_relaxed);
    }
    bool get_last_capture_used_hw() const { return m_last_capture_used_hw; }

private:
    bool m_hw_capture_supported = false;
    bool m_hw_capture_active = false;
    bool m_lock_capture_backend = false;
    bool m_locked_use_hw_capture = false;

    std::atomic<uint64_t> m_force_software_capture_until_unix_ms{0};
    uint32_t m_capture_degrade_ms = 2500;

    int m_top_black_strip_streak = 0;
    int m_top_black_strip_force_sw_threshold = 1;
    int m_top_black_strip_disable_dxgi_threshold = 3;
    bool m_disable_dxgi_for_session = false;

    bool m_last_capture_used_hw = false;
    int m_dxgi_instability_score = 0;
    int m_dxgi_disable_score_threshold = 5;

    int m_dxgi_fail_streak = 0;
    int m_dxgi_fail_reset_threshold = 6;
};

