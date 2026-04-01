#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>
#include <rtc/rtc.hpp>

struct AVCodecContext;

std::optional<std::vector<std::byte>> parse_avcc_spspps(const AVCodecContext* ctx);

void inspect_h264_avcc_sample(const rtc::binary& sample, bool& hasIdr, bool& hasSps, bool& hasPps);

bool validate_h264_avcc_payload(const uint8_t* data, size_t size);
