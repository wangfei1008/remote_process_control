#pragma once
#include <vector>
#include <cstdint>
#include <functional>

class RtpH264Packer {
public:
    using RtpCallback = std::function<void(const uint8_t* rtp, size_t len, bool marker)>;

    RtpH264Packer(uint8_t payloadType = 96, size_t mtu = 1200);

    void setSsrc(uint32_t ssrc);
    void setSequence(uint16_t seq);
    void setTimestamp(uint32_t timestamp);
    void pack(const uint8_t* nal, size_t len, RtpCallback cb);

private:
    uint8_t payloadType;
    size_t mtu;
    uint32_t ssrc = 12345;
    uint16_t seq = 0;
    uint32_t timestamp = 0;

    void writeHeader(std::vector<uint8_t>& rtp, bool marker);
};

