#include "h264_avcc_utils.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

std::optional<std::vector<std::byte>> parse_avcc_spspps(const AVCodecContext* ctx)
{
    if (!ctx || !ctx->extradata || ctx->extradata_size <= 0) return std::nullopt;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(ctx->extradata);
    const size_t size = static_cast<size_t>(ctx->extradata_size);
    if (size < 7) return std::nullopt;
    if (p[0] != 1) return std::nullopt;

    size_t off = 5;
    if (off >= size) return std::nullopt;

    const uint8_t numSps = p[off] & 0x1F;
    off += 1;

    std::vector<std::byte> out;
    auto append_nalu = [&](const uint8_t* nalu, size_t naluSize) {
        if (!nalu || naluSize == 0) return;
        const uint32_t len = static_cast<uint32_t>(naluSize);
        const uint32_t len_be =
            (len & 0x000000FFu) << 24 |
            (len & 0x0000FF00u) << 8 |
            (len & 0x00FF0000u) >> 8 |
            (len & 0xFF000000u) >> 24;
        const std::byte* lenBytes = reinterpret_cast<const std::byte*>(&len_be);
        out.insert(out.end(), lenBytes, lenBytes + 4);
        const std::byte* dataBytes = reinterpret_cast<const std::byte*>(nalu);
        out.insert(out.end(), dataBytes, dataBytes + naluSize);
    };

    for (uint8_t i = 0; i < numSps; ++i) {
        if (off + 2 > size) return std::nullopt;
        const uint16_t spsLen = (uint16_t(p[off]) << 8) | uint16_t(p[off + 1]);
        off += 2;
        if (off + spsLen > size) return std::nullopt;
        append_nalu(p + off, spsLen);
        off += spsLen;
    }

    if (off + 1 > size) return std::nullopt;
    const uint8_t numPps = p[off];
    off += 1;

    for (uint8_t i = 0; i < numPps; ++i) {
        if (off + 2 > size) return std::nullopt;
        const uint16_t ppsLen = (uint16_t(p[off]) << 8) | uint16_t(p[off + 1]);
        off += 2;
        if (off + ppsLen > size) return std::nullopt;
        append_nalu(p + off, ppsLen);
        off += ppsLen;
    }

    if (out.empty()) return std::nullopt;
    return out;
}

void inspect_h264_avcc_sample(const rtc::binary& sample, bool& hasIdr, bool& hasSps, bool& hasPps)
{
    hasIdr = false;
    hasSps = false;
    hasPps = false;
    size_t i = 0;
    while (i + 4 <= sample.size()) {
        const uint8_t b0 = static_cast<uint8_t>(sample[i + 0]);
        const uint8_t b1 = static_cast<uint8_t>(sample[i + 1]);
        const uint8_t b2 = static_cast<uint8_t>(sample[i + 2]);
        const uint8_t b3 = static_cast<uint8_t>(sample[i + 3]);
        const uint32_t length = (uint32_t(b0) << 24) | (uint32_t(b1) << 16) | (uint32_t(b2) << 8) | uint32_t(b3);
        const size_t naluStart = i + 4;
        const size_t naluEnd = naluStart + static_cast<size_t>(length);
        if (naluEnd > sample.size() || naluStart >= sample.size()) break;
        const auto* header = reinterpret_cast<const rtc::NalUnitHeader*>(sample.data() + naluStart);
        const uint8_t t = header->unitType();
        if (t == 5) hasIdr = true;
        else if (t == 7) hasSps = true;
        else if (t == 8) hasPps = true;
        i = naluEnd;
    }
}

bool validate_h264_avcc_payload(const uint8_t* data, size_t size)
{
    if (!data || size < 5) return false;
    size_t i = 0;
    bool hasVcl = false;
    while (i + 4 <= size) {
        const uint8_t b0 = data[i + 0];
        const uint8_t b1 = data[i + 1];
        const uint8_t b2 = data[i + 2];
        const uint8_t b3 = data[i + 3];
        const uint32_t naluLen = (uint32_t(b0) << 24) | (uint32_t(b1) << 16) | (uint32_t(b2) << 8) | uint32_t(b3);
        if (naluLen == 0) return false;
        constexpr uint32_t kMaxReasonableNalu = 2u * 1024u * 1024u;
        if (naluLen > kMaxReasonableNalu) return false;
        const size_t naluStart = i + 4;
        const size_t naluEnd = naluStart + static_cast<size_t>(naluLen);
        if (naluEnd > size) return false;
        const uint8_t naluHeader = data[naluStart];
        const uint8_t naluType = static_cast<uint8_t>(naluHeader & 0x1Fu);
        if (naluType == 1 || naluType == 5) hasVcl = true;
        i = naluEnd;
    }
    if (i != size) return false;
    return hasVcl;
}
