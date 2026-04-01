#pragma once

#include <cstdint>

#include "source/stream.h"

class SampleOutputState {
public:
    void reset_for_stream_start();

    void clear_current();
    void hold_or_empty(bool enable_hold);
    bool has_last_good() const { return m_have_last_good_sample; }

    void set_current(rtc::binary&& sample);
    void set_current_from_last_good_or_clear();
    void commit_current_as_last_good();

    void advance_time(uint64_t sample_duration_us);
    uint64_t sample_time_us() const { return m_sample_time_us; }
    rtc::binary current_sample() const { return m_sample; }

private:
    rtc::binary m_sample;
    uint64_t m_sample_time_us = 0;
    rtc::binary m_last_good_sample;
    bool m_have_last_good_sample = false;
};

