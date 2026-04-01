#include "capture/capture_backend_state.h"

#include <algorithm>
#include <iostream>

void CaptureBackendState::configure(bool hw_capture_supported,
                                    bool hw_capture_active,
                                    bool lock_capture_backend,
                                    bool locked_use_hw_capture,
                                    uint32_t capture_degrade_ms)
{
    m_hw_capture_supported = hw_capture_supported;
    m_hw_capture_active = hw_capture_active;
    m_lock_capture_backend = lock_capture_backend;
    m_locked_use_hw_capture = locked_use_hw_capture;
    m_capture_degrade_ms = capture_degrade_ms;
}

void CaptureBackendState::reset_for_stream_start()
{
    m_force_software_capture_until_unix_ms.store(0, std::memory_order_relaxed);
    m_dxgi_fail_streak = 0;
    m_top_black_strip_streak = 0;
    m_disable_dxgi_for_session = false;
    m_last_capture_used_hw = false;
    m_dxgi_instability_score = 0;
}

bool CaptureBackendState::decide_use_hw_capture(uint64_t cap_now_ms)
{
    const uint64_t force_sw_until = m_force_software_capture_until_unix_ms.load(std::memory_order_relaxed);
    bool use_hw_capture = false;
    if (m_lock_capture_backend) {
        use_hw_capture = m_hw_capture_supported && m_locked_use_hw_capture;
    } else {
        use_hw_capture = m_hw_capture_active && !m_disable_dxgi_for_session && (cap_now_ms >= force_sw_until);
    }
    m_last_capture_used_hw = use_hw_capture;
    return use_hw_capture;
}

void CaptureBackendState::on_dxgi_empty(uint64_t now_ms, bool& should_reset_duplication)
{
    should_reset_duplication = false;
    ++m_dxgi_fail_streak;
    m_dxgi_instability_score = (std::min)(m_dxgi_instability_score + 2, 1000);
    if (m_dxgi_fail_streak >= m_dxgi_fail_reset_threshold) {
        m_dxgi_fail_streak = 0;
        should_reset_duplication = true;
    }

    if (m_lock_capture_backend) return;

    m_force_software_capture_until_unix_ms.store(
        now_ms + static_cast<uint64_t>(m_capture_degrade_ms),
        std::memory_order_relaxed);

    if (!m_disable_dxgi_for_session &&
        m_dxgi_instability_score >= m_dxgi_disable_score_threshold) {
        m_disable_dxgi_for_session = true;
        std::cout << "[capture] dxgi disabled for this session due to repeated instability score="
                  << m_dxgi_instability_score << "\n";
    }
}

void CaptureBackendState::on_dxgi_success()
{
    m_dxgi_fail_streak = 0;
    if (m_dxgi_instability_score > 0) {
        m_dxgi_instability_score = (std::max)(0, m_dxgi_instability_score - 1);
    }
}

void CaptureBackendState::on_slow_capture(uint64_t now_ms, bool use_hw_capture)
{
    if (!use_hw_capture || m_lock_capture_backend) return;

    m_dxgi_instability_score = (std::min)(m_dxgi_instability_score + 1, 1000);
    m_force_software_capture_until_unix_ms.store(
        now_ms + static_cast<uint64_t>(m_capture_degrade_ms), std::memory_order_relaxed);

    if (!m_disable_dxgi_for_session &&
        m_dxgi_instability_score >= m_dxgi_disable_score_threshold) {
        m_disable_dxgi_for_session = true;
        std::cout << "[capture] dxgi disabled for this session due to repeated slow-capture score="
                  << m_dxgi_instability_score << "\n";
    }
}

void CaptureBackendState::on_top_black_strip_detected(uint64_t now_ms, bool hw_capture_active)
{
    ++m_top_black_strip_streak;
    if (hw_capture_active) {
        constexpr uint64_t k_long_force_sw_ms = 8000;
        m_force_software_capture_until_unix_ms.store(now_ms + k_long_force_sw_ms, std::memory_order_relaxed);
    }
    if (!m_disable_dxgi_for_session &&
        m_top_black_strip_streak >= m_top_black_strip_disable_dxgi_threshold) {
        m_disable_dxgi_for_session = true;
        std::cout << "[capture] dxgi disabled for this session due to repeated top black strip\n";
    }
}

void CaptureBackendState::on_no_top_black_strip()
{
    if (m_top_black_strip_streak > 0) --m_top_black_strip_streak;
}

