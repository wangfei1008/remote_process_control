#include "capture/capture_backend_state.h"

#include <algorithm>
#include <iostream>

void CaptureBackendState::configure(bool session_uses_dxgi, uint32_t capture_degrade_ms, bool allow_auto_dxgi_disable)
{
    m_session_uses_dxgi = session_uses_dxgi;
    m_capture_degrade_ms = capture_degrade_ms;
    m_allow_auto_dxgi_disable = allow_auto_dxgi_disable;
}

void CaptureBackendState::reset_for_stream_start()
{
    m_dxgi_fail_streak = 0;
    m_top_black_strip_streak = 0;
    m_disable_dxgi_for_session = false;
    m_last_capture_used_hw = false;
    m_dxgi_instability_score = 0;
}

void CaptureBackendState::on_dxgi_empty(uint64_t /*now_ms*/, bool& should_reset_duplication)
{
    should_reset_duplication = false;
    ++m_dxgi_fail_streak;
    m_dxgi_instability_score = (std::min)(m_dxgi_instability_score + 2, 1000);
    if (m_dxgi_fail_streak >= m_dxgi_fail_reset_threshold) {
        m_dxgi_fail_streak = 0;
        should_reset_duplication = true;
    }

    if (m_allow_auto_dxgi_disable && !m_disable_dxgi_for_session &&
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

void CaptureBackendState::on_slow_capture(uint64_t /*now_ms*/)
{
    if (!m_session_uses_dxgi) return;

    m_dxgi_instability_score = (std::min)(m_dxgi_instability_score + 1, 1000);

    if (m_allow_auto_dxgi_disable && !m_disable_dxgi_for_session &&
        m_dxgi_instability_score >= m_dxgi_disable_score_threshold) {
        m_disable_dxgi_for_session = true;
        std::cout << "[capture] dxgi disabled for this session due to repeated slow-capture score="
                  << m_dxgi_instability_score << "\n";
    }
}

void CaptureBackendState::on_top_black_strip_detected(uint64_t /*now_ms*/, bool /*hw_capture_active*/)
{
    ++m_top_black_strip_streak;
    if (m_allow_auto_dxgi_disable && !m_disable_dxgi_for_session &&
        m_top_black_strip_streak >= m_top_black_strip_disable_dxgi_threshold) {
        m_disable_dxgi_for_session = true;
        std::cout << "[capture] dxgi disabled for this session due to repeated top black strip\n";
    }
}

void CaptureBackendState::on_no_top_black_strip()
{
    if (m_top_black_strip_streak > 0) --m_top_black_strip_streak;
}
