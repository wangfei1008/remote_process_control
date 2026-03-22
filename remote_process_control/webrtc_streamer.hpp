#pragma once
#include <memory>
#include <string>
#include <rtc/rtc.hpp>
#include "rtp_h264_packer.hpp"

class WebRTCStreamer {
public:
    WebRTCStreamer();
    void start(const std::string& signalingUrl);
    void pushH264Frame(const uint8_t* data, size_t size);

private:
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> videoTrack;
    std::shared_ptr<rtc::WebSocket> ws;

    RtpH264Packer rtpPacker;
    std::string signalingUrl;

    void setupPeerConnection();
    void handleSignaling();
};
