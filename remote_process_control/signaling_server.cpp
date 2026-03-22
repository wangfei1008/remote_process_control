#include "signaling_server.hpp"
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

SignalingServer::SignalingServer() {
    rtc::InitLogger(rtc::LogLevel::Info);

    rtc::Configuration config;
    // ICE servers 可以加这里
    pc = std::make_shared<rtc::PeerConnection>(config);

    pc->onLocalDescription([this](rtc::Description description) {
        std::unique_lock<std::mutex> lock(mtx);
        answer_sdp = std::string(description);
        answer_ready = true;
        cv.notify_all();
        std::cout << "Local SDP (Answer) generated:\n" << answer_sdp << std::endl;
        });

    pc->onStateChange([](rtc::PeerConnection::State state) {
        std::cout << "PeerConnection State changed: " << (int)state << std::endl;
        });

    pc->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
        std::cout << "Gathering State changed: " << (int)state << std::endl;
        });

    pc->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
        std::cout << "New DataChannel: " << dc->label() << std::endl;
        });
}

void SignalingServer::handleOffer(const std::string& sdp) {
    rtc::Description offer(sdp, rtc::Description::Type::Offer);
    pc->setRemoteDescription(offer);
    pc->setLocalDescription();
}

void SignalingServer::start(int port) {
    httplib::Server svr;

    svr.Post("/offer", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto j = json::parse(req.body);
            if (!j.contains("sdp")) {
                res.status = 400;
                res.set_content("Missing sdp field", "text/plain");
                return;
            }

            std::string sdp = j["sdp"];
            std::cout << "Received SDP Offer:\n" << sdp << std::endl;

            handleOffer(sdp);

            // 等待 answer SDP 生成
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this]() { return answer_ready; });
            }

            json answer_json = {
                {"type", "answer"},
                {"sdp", answer_sdp}
            };
            res.set_content(answer_json.dump(), "application/json");
        }
        catch (...) {
            res.status = 500;
            res.set_content("Internal Server Error", "text/plain");
        }
        });

    std::cout << "Signaling server started on port " << port << std::endl;
    svr.listen("0.0.0.0", port);
}

