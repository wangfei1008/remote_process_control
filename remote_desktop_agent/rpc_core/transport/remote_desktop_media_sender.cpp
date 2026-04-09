#include "transport/remote_desktop_media_sender.h"

#include "transport/client_peer_connection.h"

#include "nlohmann/json.hpp"

#include <chrono>
#include <iostream>

remote_desktop_media_sender::remote_desktop_media_sender(uint64_t frame_mark_interval,
                                                         uint64_t capture_health_interval,
                                                         snapshot_clients_fn snapshot_clients,
                                                         stop_if_no_clients_fn stop_if_no_clients)
    : m_snapshot_clients(std::move(snapshot_clients))
    , m_stop_if_no_clients(std::move(stop_if_no_clients))
    , m_frame_mark_interval(frame_mark_interval)
    , m_capture_health_interval(capture_health_interval)
    , m_last_log(std::chrono::steady_clock::now())
{
}

void remote_desktop_media_sender::on_video_sample(uint64_t video_sample_time_us,
                                                   const rtc::binary& video_sample,
                                                   const remote_capture_telemetry& telemetry)
{
    const auto clients = m_snapshot_clients ? m_snapshot_clients() : std::vector<std::shared_ptr<ClientPeerConnection>>{};

    // 1) 视频遥测/控制信令
    send_video_control_messages(clients, video_sample_time_us, video_sample, telemetry);

    // 2) RTP 视频帧
    send_media_frames(true, video_sample, video_sample_time_us, clients);

    // 3) stop_if_no_clients（保持原行为：每次 on_* 后检查）
    if (m_stop_if_no_clients) m_stop_if_no_clients();
}

void remote_desktop_media_sender::on_audio_sample(uint64_t audio_sample_time_us,
                                                   const rtc::binary& audio_sample)
{
    const auto clients = m_snapshot_clients ? m_snapshot_clients() : std::vector<std::shared_ptr<ClientPeerConnection>>{};

    send_media_frames(false, audio_sample, audio_sample_time_us, clients);

    if (m_stop_if_no_clients) m_stop_if_no_clients();
}

void remote_desktop_media_sender::send_video_control_messages(
    const std::vector<std::shared_ptr<ClientPeerConnection>>& clients,
    uint64_t video_sample_time_us,
    const rtc::binary& video_sample,
    const remote_capture_telemetry& telemetry)
{
    m_video_frame_index++;

    // 分辨率变化：依赖 telemetry（阶段性先用 0/真实由后续 engine 填充）
    const int curW = telemetry.capture_width;
    const int curH = telemetry.capture_height;
    if (curW > 0 && curH > 0 && (curW != m_last_video_w || curH != m_last_video_h)) {
        m_last_video_w = curW;
        m_last_video_h = curH;
        const nlohmann::json vres = {
            { "type", "videoResolution" },
            { "width", curW },
            { "height", curH },
        };
        const std::string payload = vres.dump();
        for (const auto& client : clients) {
            auto ch = client ? client->get_data_channel() : nullptr;
            if (!ch) continue;
            try { ch->send(payload); } catch (...) {}
        }
    }

    // 每秒一次 debug
    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_log > std::chrono::seconds(1)) {
        m_last_log = now;
        std::cout << "[video] sampleTime(us)=" << video_sample_time_us << " size=" << video_sample.size() << std::endl;
        std::cout << "[latency][sender] capture_ms=" << telemetry.last_capture_ms
                  << " encode_ms=" << telemetry.last_encode_ms
                  << " frame_unix_ms=" << telemetry.last_frame_unix_ms << std::endl;
    }

    // frameMark / captureHealth：按间隔发送
    if ((m_video_frame_index % m_frame_mark_interval) != 0) return;

    uint64_t srv_wall = telemetry.last_frame_unix_ms != 0 ? telemetry.last_frame_unix_ms : 0;
    if (srv_wall == 0) {
        srv_wall = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    const nlohmann::json mark = {
        { "type", "frameMark" },
        { "seq", m_video_frame_index },
        { "srvMs", srv_wall },
        { "capMs", telemetry.last_capture_ms },
        { "encMs", telemetry.last_encode_ms },
    };
    const std::string payload = mark.dump();
    for (const auto& client : clients) {
        auto ch = client ? client->get_data_channel() : nullptr;
        if (!ch) continue;
        try { ch->send(payload); } catch (...) {}
    }

    if ((m_video_frame_index % m_capture_health_interval) == 0) {
        const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const bool forceSwActive = (telemetry.force_software_capture_until_unix_ms != 0 &&
                                      nowMs < telemetry.force_software_capture_until_unix_ms);

        const nlohmann::json health = {
            { "type", "captureHealth" },
            { "backend", telemetry.last_capture_used_hw ? "dxgi" : "gdi" },
            { "dxgiDisabledForSession", telemetry.dxgi_disabled_for_session },
            { "topBlackStripStreak", telemetry.top_black_strip_streak },
            { "dxgiInstabilityScore", telemetry.dxgi_instability_score },
            { "forceSoftwareActive", forceSwActive },
            { "forceSoftwareRemainMs", forceSwActive ? (telemetry.force_software_capture_until_unix_ms - nowMs) : 0 },
            { "capMs", telemetry.last_capture_ms },
            { "encMs", telemetry.last_encode_ms },
        };

        const std::string healthPayload = health.dump();
        for (const auto& client : clients) {
            auto ch = client ? client->get_data_channel() : nullptr;
            if (!ch) continue;
            try { ch->send(healthPayload); } catch (...) {}
        }
    }
}

void remote_desktop_media_sender::send_media_frames(
    bool is_video,
    const rtc::binary& sample,
    uint64_t sample_time_us,
    const std::vector<std::shared_ptr<ClientPeerConnection>>& clients)
{
    // 视频空样本跳过（与原逻辑一致）
    if (is_video && sample.empty()) return;

    const auto sampleDuration = std::chrono::duration<double, std::micro>(static_cast<double>(sample_time_us));

    for (const auto& client : clients) {
        if (!client) continue;
        if (client->get_state() != ClientPeerConnection::State::Ready) continue;

        const std::optional<std::shared_ptr<ClientTrackData>> optTrackData =
            is_video ? client->get_video_track_data() : client->get_audio_track_data();
        if (!optTrackData.has_value()) continue;

        try {
            optTrackData.value()->track->sendFrame(sample, sampleDuration);
        } catch (...) {}
    }
}


