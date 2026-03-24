#include "webrtc_streamer.hpp"
#include <iostream>

WebRTCStreamer::WebRTCStreamer() {
    rtc::InitLogger(rtc::LogLevel::Info);
    rtpPacker.set_ssrc(12345678); // arbitrary SSRC
    rtpPacker.set_sequence(0);
}

void WebRTCStreamer::start(const std::string& url) {
    signalingUrl = url;
    setup_peer_connection();
    handle_signaling();  
    
}

void WebRTCStreamer::setup_peer_connection() {
    rtc::Configuration config;
    config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    config.disableAutoNegotiation = false;
    pc = std::make_shared<rtc::PeerConnection>(config);

    auto video = rtc::Description::Video("video");
    video.addH264Codec(102);
    video.addSSRC(1, "video-stream", "stream1", "video");
    videoTrack = pc->addTrack(video);
    //videoTrack = pc->addTrack(rtc::Description::Media("video", "video", rtc::Description::Direction::SendOnly));

    pc->onLocalDescription([this](rtc::Description desc) {
        if (ws && ws->isOpen()) {
            std::string sdp = std::string(desc);
            ws->send(sdp);
        }
        });

    pc->onLocalCandidate([this](rtc::Candidate cand) {
        if (ws && ws->isOpen()) {
            std::string candStr = std::string(cand);
            ws->send(candStr);
        }
        });

    pc->onStateChange([](rtc::PeerConnection::State state) {
        std::cout << "PeerConnection state: " << static_cast<int>(state) << std::endl;
        });
}

void WebRTCStreamer::handle_signaling() {
   //ws = std::make_unique<rtc::WebSocket>(signalingUrl);
    ws = std::make_shared<rtc::WebSocket>();

    ws->onOpen([this]() {
        std::cout << "WebSocket connected to signaling server" << std::endl;
        auto offer = pc->createOffer();
        rtc::LocalDescriptionInit init; // Create an empty LocalDescriptionInit object
        pc->setLocalDescription(rtc::Description::Type::Offer, init);

        });

    ws->onMessage([this](rtc::message_variant message) {
        if (auto str = std::get_if<std::string>(&message)) {
            if (str->find("a=ice-ufrag") != std::string::npos || str->find("v=0") != std::string::npos) {
                rtc::Description remoteSdp(*str, rtc::Description::Type::Answer);
                pc->setRemoteDescription(remoteSdp);
            }
            else if (str->find("candidate:") != std::string::npos) {
                rtc::Candidate cand(*str);
                pc->addRemoteCandidate(cand);
            }
        }
        });
    ws->open(signalingUrl);

    ws->onError([](std::string err) {
        std::cerr << "WebSocket error: " << err << std::endl;
        });

    ws->onClosed([]() {
        std::cerr << "WebSocket closed" << std::endl;
        });
}

//void WebRTCStreamer::pushH264Frame(const uint8_t* data, size_t size) {
//    if (!videoTrack) return;
//
//    size_t pos = 0;
//    while (pos + 4 < size) {
//        size_t start = pos;
//        while (pos + 4 < size && !(data[pos] == 0 && data[pos + 1] == 0 &&
//            ((data[pos + 2] == 1) || (data[pos + 2] == 0 && data[pos + 3] == 1)))) {
//            pos++;
//        }
//
//        size_t nal_start = 0;
//        size_t nal_size = 0;
//        if (data[start + 2] == 1) {
//            nal_start = start + 3;
//        }
//        else if (data[start + 3] == 1) {
//            nal_start = start + 4;
//        }
//
//        size_t next = pos;
//        if (pos + 4 >= size) next = size;
//
//        nal_size = next - nal_start;
//
//        if (nal_size > 0 && nal_start + nal_size <= size) {
//            auto now = std::chrono::steady_clock::now();
//            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() * 90;
//
//            rtpPacker.setTimestamp(static_cast<uint32_t>(timestamp));
//            rtpPacker.pack(data + nal_start, nal_size, [this](const uint8_t* rtp, size_t len, bool marker) {
//                std::vector<uint8_t> rtpPacket(rtp, rtp + len); // Convert raw pointer to std::vector
//                videoTrack->send(reinterpret_cast<const std::byte*>(rtpPacket.data()), rtpPacket.size()); // Convert uint8_t* to const char*
//                });
//        }
//        pos = next;
//    }
//}

void WebRTCStreamer::push_h264_frame(const uint8_t* data, size_t size) {
    if (!videoTrack || !data || size < 4) return;

    size_t i = 0;
    while (i + 4 < size) {
        // Find NAL start code (0x00000001 or 0x000001).
        size_t start = i;
        size_t nal_start = std::string::npos;
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                nal_start = i + 3;
            }
            else if (data[i + 2] == 0 && data[i + 3] == 1) {
                nal_start = i + 4;
            }
        }

        if (nal_start == std::string::npos) {
            i++;
            continue;
        }

        // Find the next start code.
        size_t next_nal = nal_start;
        while (next_nal + 4 < size) {
            if (data[next_nal] == 0 && data[next_nal + 1] == 0 &&
                ((data[next_nal + 2] == 1) || (data[next_nal + 2] == 0 && data[next_nal + 3] == 1))) {
                break;
            }
            ++next_nal;
        }

        size_t nal_size = next_nal - nal_start;

        // Valid NALU.
        if (nal_size > 0 && (nal_start + nal_size <= size)) {
            // Packetize as RTP and send.
            auto now = std::chrono::steady_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() * 90;

            rtpPacker.set_timestamp(static_cast<uint32_t>(timestamp));
            rtpPacker.pack(data + nal_start, nal_size, [this](const uint8_t* rtp, size_t len, bool marker) {
                std::vector<uint8_t> rtpPacket(rtp, rtp + len); // Convert raw pointer to std::vector
                videoTrack->send(reinterpret_cast<const std::byte*>(rtpPacket.data()), rtpPacket.size()); // Convert uint8_t* to const char*
                });
        }

        i = next_nal;
    }
}
