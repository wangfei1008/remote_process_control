#include "desktop_capture.hpp"
#include "ffmpeg_encoder.hpp"
#include "webrtc_streamer.hpp"
//#include "signaling_server.hpp"

#include <thread>
#include <chrono>
#include <iostream>

int main() {
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    const int fps = 30;

    DesktopCapture capture;
    FFmpegEncoder encoder(width, height, fps);
    WebRTCStreamer streamer;
    //SignalingServer signaling(8000);

   // std::thread signalingThread([&]() { signaling.start(); });

    streamer.start("ws://127.0.0.1:8000/server");  // Connect to local signaling server.

    int frameSize = width * height * 3;
    std::vector<uint8_t> rgbBuffer(frameSize);

    while (true) {
        if (!capture.captureFrame(rgbBuffer.data(), width, height)) {
            std::cerr << "Capture failed" << std::endl;
            continue;
        }

        if (!encoder.encode_frame(rgbBuffer.data(), frameSize)) {
            std::cerr << "Encode failed" << std::endl;
            continue;
        }

        AVPacket* pkt = encoder.get_packet();
        if (pkt && pkt->size > 0) {
            streamer.push_h264_frame(pkt->data, pkt->size);
            encoder.free_packet();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // 30fps
    }

    //signalingThread.join();
    return 0;
}

