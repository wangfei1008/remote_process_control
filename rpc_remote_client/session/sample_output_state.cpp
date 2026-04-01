#include "session/sample_output_state.h"

void SampleOutputState::reset_for_stream_start()
{
    m_sample.clear();
    m_sample_time_us = 0;
    m_last_good_sample.clear();
    m_have_last_good_sample = false;
}

void SampleOutputState::clear_current()
{
    m_sample.clear();
}

void SampleOutputState::hold_or_empty(bool enable_hold)
{
    if (enable_hold && m_have_last_good_sample) {
        m_sample = m_last_good_sample;
    } else {
        m_sample.clear();
    }
}

void SampleOutputState::set_current(rtc::binary&& sample)
{
    m_sample = std::move(sample);
}

void SampleOutputState::set_current_from_last_good_or_clear()
{
    if (m_have_last_good_sample) {
        m_sample = m_last_good_sample;
    } else {
        m_sample.clear();
    }
}

void SampleOutputState::commit_current_as_last_good()
{
    m_last_good_sample = m_sample;
    m_have_last_good_sample = true;
}

void SampleOutputState::advance_time(uint64_t sample_duration_us)
{
    m_sample_time_us += sample_duration_us;
}

