#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <chrono>

#include "rtc/rtc.hpp"

#include "session/remote_capture_telemetry.h"

class ClientPeerConnection;
struct ClientTrackData;

class remote_desktop_media_sender {
public:
    using snapshot_clients_fn = std::function<std::vector<std::shared_ptr<ClientPeerConnection>>()>; // 单会话：0 或 1 个客户端
    using stop_if_no_clients_fn = std::function<void()>;

    remote_desktop_media_sender(uint64_t frame_mark_interval,
                                 uint64_t capture_health_interval,
                                 snapshot_clients_fn snapshot_clients,
                                 stop_if_no_clients_fn stop_if_no_clients);

    void on_video_sample(uint64_t video_sample_time_us,
                          const rtc::binary& video_sample,
                          const remote_capture_telemetry& telemetry);

    void on_audio_sample(uint64_t audio_sample_time_us,
                          const rtc::binary& audio_sample);

private:
    void send_video_control_messages(const std::vector<std::shared_ptr<ClientPeerConnection>>& clients,
                                       uint64_t video_sample_time_us,
                                       const rtc::binary& video_sample,
                                       const remote_capture_telemetry& telemetry);

    void send_media_frames(bool is_video,
                            const rtc::binary& sample,
                            uint64_t sample_time_us,
                            const std::vector<std::shared_ptr<ClientPeerConnection>>& clients);

private:
    snapshot_clients_fn m_snapshot_clients;
    stop_if_no_clients_fn m_stop_if_no_clients;

    uint64_t m_frame_mark_interval = 0;
    uint64_t m_capture_health_interval = 0;

    std::chrono::steady_clock::time_point m_last_log;
    uint64_t m_video_frame_index = 0;
    int m_last_video_w = 0;
    int m_last_video_h = 0;
};

