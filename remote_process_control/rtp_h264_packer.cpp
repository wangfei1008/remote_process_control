#include "rtp_h264_packer.hpp"

RtpH264Packer::RtpH264Packer(uint8_t payloadType, size_t mtu)
    : payloadType(payloadType), mtu(mtu) {
}

void RtpH264Packer::setSsrc(uint32_t s) { ssrc = s; }
void RtpH264Packer::setSequence(uint16_t s) { seq = s; }
void RtpH264Packer::setTimestamp(uint32_t t) { timestamp = t; }

void RtpH264Packer::writeHeader(std::vector<uint8_t>& rtp, bool marker) {
    rtp.resize(12);
    rtp[0] = 0x80;
    rtp[1] = payloadType | (marker ? 0x80 : 0x00);
    rtp[2] = seq >> 8;
    rtp[3] = seq & 0xFF;
    rtp[4] = timestamp >> 24;
    rtp[5] = (timestamp >> 16) & 0xFF;
    rtp[6] = (timestamp >> 8) & 0xFF;
    rtp[7] = timestamp & 0xFF;
    rtp[8] = ssrc >> 24;
    rtp[9] = (ssrc >> 16) & 0xFF;
    rtp[10] = (ssrc >> 8) & 0xFF;
    rtp[11] = ssrc & 0xFF;
}

void RtpH264Packer::pack(const uint8_t* nal, size_t len, RtpCallback cb) {
    if (len < 2) return;

    const uint8_t nal_header = nal[0];
    const uint8_t nal_type = nal_header & 0x1F;

    if (len <= mtu - 12) {
        std::vector<uint8_t> rtp;
        writeHeader(rtp, true);
        rtp.insert(rtp.end(), nal, nal + len);
        cb(rtp.data(), rtp.size(), true);
        seq++;
    }
    else {
        size_t payload_max = mtu - 14;
        size_t offset = 1;

        while (offset < len) {
            size_t size = std::min(len - offset, payload_max);
            std::vector<uint8_t> rtp;
            bool start = (offset == 1);
            bool end = (offset + size >= len);
            writeHeader(rtp, end);

            uint8_t fu_indicator = (nal_header & 0xE0) | 28;
            uint8_t fu_header = (start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | nal_type;

            rtp.push_back(fu_indicator);
            rtp.push_back(fu_header);
            rtp.insert(rtp.end(), nal + offset, nal + offset + size);

            cb(rtp.data(), rtp.size(), end);
            offset += size;
            seq++;
        }
    }
}

