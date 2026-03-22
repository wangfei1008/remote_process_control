#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <condition_variable>
#include <rtc/rtc.hpp>

class SignalingServer {
public:
    SignalingServer();
    void start(int port);

private:
    std::shared_ptr<rtc::PeerConnection> pc;
    std::mutex mtx;
    std::condition_variable cv;
    std::string answer_sdp;
    bool answer_ready = false;

    void handleOffer(const std::string& sdp);
};
