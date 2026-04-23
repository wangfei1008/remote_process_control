#include "transport/remote_desktop_media_sender.h"

#include "transport/client_peer_connection.h"

#include "nlohmann/json.hpp"

#include "common/rpc_time.h"

#include <array>
#include <cstddef>
#include <chrono>
#include <iostream>
#include <sstream>
#include <vector>

static std::string format_unix_ms_local(uint64_t unix_ms)
{
    const std::time_t sec = static_cast<std::time_t>(unix_ms / 1000);
    const int ms = static_cast<int>(unix_ms % 1000);
    std::tm tm_local{};
#if defined(_WIN32)
    localtime_s(&tm_local, &sec);
#else
    localtime_r(&sec, &tm_local);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms;
    return oss.str();
}

static bool LooksLikeAnnexB(const rtc::binary& b)
{
    if (b.size() >= 4) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(b.data());
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return true;
    }
    if (b.size() >= 3) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(b.data());
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) return true;
    }
    return false;
}

static inline std::byte B(uint8_t v) { return static_cast<std::byte>(v); }

static void AppendRbspWithEmulationPrevention(rtc::binary& out, const uint8_t* rbsp, size_t n)
{
    int zeroCount = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = rbsp[i];
        if (zeroCount >= 2 && b <= 0x03) {
            out.push_back(B(0x03));
            zeroCount = 0;
        }
        out.push_back(B(b));
        if (b == 0x00) zeroCount++;
        else zeroCount = 0;
    }
}

static std::vector<uint8_t> BuildRpcLatencySeiRbsp(uint64_t frameId, uint64_t capMs, uint64_t encMs, uint64_t sendMs, uint64_t prepMs)
{
    static constexpr std::array<uint8_t, 16> kUuid = {
        0x52, 0x50, 0x43, 0x2D, 0x4C, 0x41, 0x54, 0x45,
        0x4E, 0x43, 0x59, 0x2D, 0x53, 0x45, 0x49, 0x31
    }; // "RPC-LATENCY-SEI1"

    // SEI payload:
    //   UUID(16) + version(1) + frameId(8 LE) + capMs(8 LE) + encMs(8 LE) + sendMs(8 LE) + prepMs(8 LE)  [v2]
    std::vector<uint8_t> rbsp;
    rbsp.reserve(2 + 2 + 16 + 1 + 8 * 5 + 1);

    const uint32_t payloadType = 5;               // user_data_unregistered
    const uint32_t payloadSize = 16 + 1 + 8 * 5;  // version 2 == 57
    rbsp.push_back(static_cast<uint8_t>(payloadType));
    rbsp.push_back(static_cast<uint8_t>(payloadSize));
    rbsp.insert(rbsp.end(), kUuid.begin(), kUuid.end());
    rbsp.push_back(2); // version: prepMs added
    for (int i = 0; i < 8; ++i) rbsp.push_back(static_cast<uint8_t>((frameId >> (8 * i)) & 0xFF));
    for (int i = 0; i < 8; ++i) rbsp.push_back(static_cast<uint8_t>((capMs >> (8 * i)) & 0xFF));
    for (int i = 0; i < 8; ++i) rbsp.push_back(static_cast<uint8_t>((encMs >> (8 * i)) & 0xFF));
    for (int i = 0; i < 8; ++i) rbsp.push_back(static_cast<uint8_t>((sendMs >> (8 * i)) & 0xFF));
    for (int i = 0; i < 8; ++i) rbsp.push_back(static_cast<uint8_t>((prepMs >> (8 * i)) & 0xFF));
    rbsp.push_back(0x80); // rbsp_trailing_bits
    return rbsp;
}

static rtc::binary BuildRpcLatencySeiNaluAnnexB(uint64_t frameId, uint64_t capMs, uint64_t encMs, uint64_t sendMs, uint64_t prepMs)
{
    const std::vector<uint8_t> rbsp = BuildRpcLatencySeiRbsp(frameId, capMs, encMs, sendMs, prepMs);
    rtc::binary out;
    out.reserve(4 + 1 + rbsp.size() + 8);
    out.insert(out.end(), { B(0x00), B(0x00), B(0x00), B(0x01) }); // start code
    out.push_back(B(0x06));                                       // nal_unit_type=6 (SEI)
    AppendRbspWithEmulationPrevention(out, rbsp.data(), rbsp.size());
    return out;
}

static rtc::binary BuildRpcLatencySeiNaluAvcc(uint64_t frameId, uint64_t capMs, uint64_t encMs, uint64_t sendMs, uint64_t prepMs)
{
    const std::vector<uint8_t> rbsp = BuildRpcLatencySeiRbsp(frameId, capMs, encMs, sendMs, prepMs);
    rtc::binary out;
    // AVCC: 4-byte big-endian length + NAL (header+ebsp)
    // NAL header is 1 byte (0x06) + rbsp with emulation prevention.
    rtc::binary nal;
    nal.reserve(1 + rbsp.size() + 8);
    nal.push_back(B(0x06));
    AppendRbspWithEmulationPrevention(nal, rbsp.data(), rbsp.size());

    const uint32_t len = static_cast<uint32_t>(nal.size());
    out.reserve(4 + nal.size());
    out.push_back(B(static_cast<uint8_t>((len >> 24) & 0xFF)));
    out.push_back(B(static_cast<uint8_t>((len >> 16) & 0xFF)));
    out.push_back(B(static_cast<uint8_t>((len >> 8) & 0xFF)));
    out.push_back(B(static_cast<uint8_t>((len >> 0) & 0xFF)));
    out.insert(out.end(), nal.begin(), nal.end());
    return out;
}

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

void remote_desktop_media_sender::maybe_stop_if_no_clients(const std::vector<std::shared_ptr<ClientPeerConnection>>& clients)
{
    const auto now = std::chrono::steady_clock::now();
    if (!clients.empty()) {
        m_last_client_seen = now;
        m_stop_called = false;
        return;
    }

    if (!m_stop_if_no_clients) return;
    if (m_stop_called) return;

    const auto elapsed_ms =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_client_seen).count());

    // Log at most once every 5 seconds to avoid spam.
    if (now - m_last_stop_check_log >= std::chrono::seconds(5)) {
        m_last_stop_check_log = now;
        std::cout << "[media] no clients seen for " << elapsed_ms << "ms (grace=" << m_no_client_grace_ms << "ms)\n";
    }

    if (elapsed_ms < m_no_client_grace_ms) return;

    m_stop_called = true;
    std::cout << "[media] no-client grace expired, stopping media session\n";
    m_stop_if_no_clients();
}

void remote_desktop_media_sender::on_video_sample(uint64_t video_sample_time_us, const rtc::binary& video_sample, const rpc_video_contract::TelemetrySnapshot& telemetry)
{
    const auto clients = m_snapshot_clients ? m_snapshot_clients() : std::vector<std::shared_ptr<ClientPeerConnection>>{};

    // 1) 视频遥测/控制信令
    send_video_control_messages(clients, video_sample_time_us, video_sample, telemetry);

    rtc::binary sampleToSend = video_sample;
    if (!video_sample.empty()) {
        const uint64_t sendMs = rpc_unix_epoch_ms();
        uint64_t capMs = telemetry.capture_unix_ms;
        uint64_t encMs = telemetry.encode_unix_ms;
        uint64_t prepMs = telemetry.prep_unix_ms;

        const bool isAnnexB = LooksLikeAnnexB(video_sample);
        const rtc::binary sei = isAnnexB
            ? BuildRpcLatencySeiNaluAnnexB(telemetry.frame_id, capMs, encMs, sendMs, prepMs)
            : BuildRpcLatencySeiNaluAvcc(telemetry.frame_id, capMs, encMs, sendMs, prepMs);
        sampleToSend = rtc::binary{};
        sampleToSend.reserve(sei.size() + video_sample.size());
        sampleToSend.insert(sampleToSend.end(), sei.begin(), sei.end());
        sampleToSend.insert(sampleToSend.end(), video_sample.begin(), video_sample.end());
    }

    // 2) RTP 视频帧
    send_media_frames(true, sampleToSend, video_sample_time_us, clients);

    // 3) stop_if_no_clients（仅当无客户端持续超过宽限）
    maybe_stop_if_no_clients(clients);
}

void remote_desktop_media_sender::on_audio_sample(uint64_t audio_sample_time_us,
                                                   const rtc::binary& audio_sample)
{
    const auto clients = m_snapshot_clients ? m_snapshot_clients() : std::vector<std::shared_ptr<ClientPeerConnection>>{};

    send_media_frames(false, audio_sample, audio_sample_time_us, clients);

    maybe_stop_if_no_clients(clients);
}

void remote_desktop_media_sender::send_video_control_messages(const std::vector<std::shared_ptr<ClientPeerConnection>>& clients, uint64_t video_sample_time_us, const rtc::binary& video_sample, const rpc_video_contract::TelemetrySnapshot& telemetry)
{
    // 分辨率变化：依赖 telemetry（阶段性先用 0/真实由后续 engine 填充）
    const int curW = telemetry.capture_size.w;
    const int curH = telemetry.capture_size.h;
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
        std::cout << "[latency][sender] id =" << telemetry.frame_id
            << " size(" << telemetry.capture_size.w << "," << telemetry.capture_size.h << ")"
            << " frame time =" << format_unix_ms_local(telemetry.frame_unix_ms)
            << " prep time =" << format_unix_ms_local(telemetry.prep_unix_ms)
            << " cap time =" << format_unix_ms_local(telemetry.capture_unix_ms)
            << " enc time =" << format_unix_ms_local(telemetry.encode_unix_ms)
            << std::endl;
    }

    // Only emit captureHealth when we have a real video frame.
    if (video_sample.empty()) return;

    if ((telemetry.frame_id % m_capture_health_interval) == 0) {
        const nlohmann::json health = {
            { "type", "captureHealth" },
            { "backend", telemetry.backend == rpc_video_contract::CaptureBackend::Dxgi ? "dxgi" : "gdi" },
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


