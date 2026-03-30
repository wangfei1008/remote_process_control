#include "webrtc_socket.h"
#include <thread>
#include <chrono>
#include "input_controller.h"
#include "client_peer_connection.h"
#include "desktop_screen_source.h"
#include "process_manager.h"
#include "silence_opus_source.h"
#include <atomic>

using namespace std::chrono_literals;

WebRTCSocket::WebRTCSocket() 
    : m_ws(nullptr)
    , m_signaling_ip("")
    , m_signaling_url("")
    , m_thread_queue("WebRTCSocket")
{
	rtc::InitLogger(rtc::LogLevel::Info);
}

void WebRTCSocket::start(const std::string& ip, int port)
{
	InputController::ensure_process_dpi_awareness();
	m_signaling_ip = ip;
    m_signaling_port = port;
	_init_signaling();
}

void WebRTCSocket::_init_signaling()
{    
    //??????stun:stun.l.tencent.com:3478
    //??????????????????stun:stun.aliyun.com : 3478
    //google:stun:stun.l.google.com:19302
    std::string stun_server = "stun:stun.l.tencent.com:3478";

    m_config.iceServers.emplace_back(stun_server);
    m_config.disableAutoNegotiation = true;

    std::string localId = "server";

    m_ws = std::make_shared<rtc::WebSocket>();

    m_ws->onOpen([this]() { LOGINFO("url = %s, WebSocket connected, signaling ready", m_signaling_url.c_str()); });
    m_ws->onClosed([this]() {
        LOGINFO("url = %s, WebSocket closed", m_signaling_url.c_str());
        m_thread_queue.dispatch([this]() {
            _stop_and_reset_stream();
        });
    });
    m_ws->onError([this](const std::string& error) { LOGERROR("url = %s, WebSocket failed: %s", m_signaling_url.c_str(), error.c_str()); });
	m_ws->onMessage([this](const std::variant<rtc::binary, std::string>& data) 
                    {    
                        if (std::holds_alternative<std::string>(data))
                        {
                            try {
                                LOGINFO("message data = %s", std::get<std::string>(data).c_str());
                                nlohmann::json message = nlohmann::json::parse(std::get<std::string>(data));
                                m_thread_queue.dispatch([message, this](){ this->_on_message(message); });
                            } catch (const std::exception& e) {
                                std::cerr << "[signaling] invalid JSON message: " << e.what() << std::endl;
                            } catch (...) {
                                std::cerr << "[signaling] invalid JSON message: unknown error" << std::endl;
                            }
                        }
                        else
                        {
                            LOGERROR("Received binary data, which is not supported in this example.");
                        } 
                    });

    m_signaling_url = "ws://" + m_signaling_ip + ":" + std::to_string(m_signaling_port) + "/" + localId;
    std::cout << "[signaling] opening websocket url=" << m_signaling_url << std::endl;
    try {
        m_ws->open(m_signaling_url);
    } catch (const std::exception& e) {
        std::cerr << "[signaling] m_ws->open exception: " << e.what() << std::endl;
        return;
    } catch (...) {
        std::cerr << "[signaling] m_ws->open exception: unknown error" << std::endl;
        return;
    }

    LOGINFO("Waiting for signaling to be connected...");
    while (!m_ws->isOpen()) {
        if (m_ws->isClosed()) return ;
        std::this_thread::sleep_for(100ms);
    }
}

inline void WebRTCSocket::_on_message(nlohmann::json message)
{
    auto it = message.find("id");
    if (it == message.end()) return;
    std::string id = it->get<std::string>();

    it = message.find("type");
    if (it == message.end())return;

    std::string type = it->get<std::string>();
    if (type == "request") // start session request
    {
        // Always start from a clean state: each request should be a fresh session.
        _stop_and_reset_stream();

        // optional process path from frontend
        if (message.contains("exePath")) {
            const std::string exePath = message.value("exePath", "");
            if (!exePath.empty()) {
                std::scoped_lock lk(m_stream_mtx);
                m_exe_path = exePath;
                // reset stream so next start uses new process
                if (m_stream) {
                    m_stream->stop();
                    m_stream.reset();
                }
            }
        }

        auto pc = _init_peer_connection(id);
		auto client = std::make_shared<ClientPeerConnection>(
            pc, make_weak_ptr(m_ws), id,
            [this](const std::string& clientId, std::shared_ptr<rtc::DataChannel> dc) { return _request_control(clientId, dc); },
            [this](const std::string& clientId) { _release_control(clientId); },
            [this](const std::string& clientId) { return _is_controller(clientId); }
        );
        // keep original signature; stream itself is shared in _get_or_create_stream()
        client->set_callback([this](const std::string, const unsigned, const std::string) {
            return _get_or_create_stream();
        });

        m_clients.emplace(id, client);
        pc->setLocalDescription();
    }
    else if (type == "answer") // SDP answer
    {
        if (auto jt = m_clients.find(id); jt != m_clients.end()) 
        {
            auto pc = jt->second->get_peer_connection();
            auto sdp = message["sdp"].get<std::string>();
            auto description = rtc::Description(sdp, type);
            pc->setRemoteDescription(description);
        }
    }
    else if (type == "stop")
    {
        // Regardless of who initiated stop, enforce full cleanup and let frontend exit video page.
        _stop_and_reset_stream();
    }
}

std::shared_ptr<rtc::PeerConnection> WebRTCSocket::_init_peer_connection(const std::string& id)
{
    auto pc = std::make_shared<rtc::PeerConnection>(m_config);

    pc->onStateChange([id, this](rtc::PeerConnection::State state)
        {
            if (state == rtc::PeerConnection::State::Disconnected ||
                state == rtc::PeerConnection::State::Failed ||
                state == rtc::PeerConnection::State::Closed) {
                // remove disconnected client
                m_thread_queue.dispatch([id, this]() {
                    m_clients.erase(id);
                    _release_control(id);
                    // Strong lifecycle guarantee:
                    // when frontend closes page/stream and no clients remain,
                    // immediately tear down process/stream resources.
                    if (m_clients.empty()) {
                        _stop_and_reset_stream();
                    }
                });
            }
        });

    pc->onGatheringStateChange(
        [wpc = make_weak_ptr(pc), id, this](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                if (auto pc = wpc.lock()) {
                    auto description = pc->localDescription();
                    nlohmann::json message = {
                        {"id", id},
                        {"type", description->typeString()},
                        {"sdp", std::string(description.value())}
                    };
                    // Gathering complete, send answer
					auto ws = make_weak_ptr(m_ws);
                    if (auto wws = ws.lock()) {
                        wws->send(message.dump());
                    }
                }
            }
        });

    return pc;
}

std::shared_ptr<Stream> WebRTCSocket::_get_or_create_stream()
{
    {
        std::scoped_lock lk(m_stream_mtx);
        if (m_stream) return m_stream;
    }

    std::cout << "[stream] _get_or_create_stream begin" << std::endl;

    const std::string exePath = m_exe_path.empty() ? "C:\\Windows\\System32\\notepad.exe" : m_exe_path;
    std::cout << "[stream] launching exePath=" << exePath << std::endl;

    auto video = std::make_shared<ProcessManager>();
    video->set_on_remote_exit([this]() {
        m_thread_queue.dispatch([this]() {
            _stop_and_reset_stream();
        });
    });
    video->launch_process(exePath);
    auto audio = std::make_shared<SilenceOpusSource>();

    auto stream = std::make_shared<Stream>(video, audio);
    // set callback responsible for sample sending
    stream->on_sample([ws = make_weak_ptr(stream), this](Stream::StreamSourceType type, uint64_t sampleTime, rtc::binary sample) {
        static auto lastLog = std::chrono::steady_clock::now();
        static uint64_t videoFrameIndex = 0;
        if (type == Stream::StreamSourceType::Video) {
            videoFrameIndex++;
            auto now = std::chrono::steady_clock::now();
            if (now - lastLog > std::chrono::seconds(1)) {
                lastLog = now;
                std::cout << "[video] sampleTime(us)=" << sampleTime << " size=" << sample.size() << std::endl;
                if (auto s = ws.lock()) {
                    if (auto pm = std::dynamic_pointer_cast<ProcessManager>(s->m_c_video)) {
                        std::cout << "[latency][sender] capture_ms=" << pm->get_last_capture_ms()
                                  << " encode_ms=" << pm->get_last_encode_ms()
                                  << " frame_unix_ms=" << pm->get_last_frame_unix_ms() << std::endl;
                    }
                }
            }

            // Send frameMark at a lower frequency to reduce DataChannel/JSON overhead.
            constexpr uint64_t kFrameMarkInterval = 10;
            if ((videoFrameIndex % kFrameMarkInterval) == 0) {
                uint64_t srv_wall = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
                uint32_t cap_ms = 0;
                uint32_t enc_ms = 0;
                bool capHw = false;
                bool dxgiDisabled = false;
                int topBlackStripStreak = 0;
                int dxgiInstabilityScore = 0;
                uint64_t forceSwUntilMs = 0;
                if (auto s = ws.lock()) {
                    if (auto pm = std::dynamic_pointer_cast<ProcessManager>(s->m_c_video)) {
                        cap_ms = pm->get_last_capture_ms();
                        enc_ms = pm->get_last_encode_ms();
                        const uint64_t fu = pm->get_last_frame_unix_ms();
                        if (fu != 0) srv_wall = fu;
                        capHw = pm->get_last_capture_used_hw();
                        dxgiDisabled = pm->is_dxgi_disabled_for_session();
                        topBlackStripStreak = pm->get_top_black_strip_streak();
                        dxgiInstabilityScore = pm->get_dxgi_instability_score();
                        forceSwUntilMs = pm->get_force_software_capture_until_unix_ms();
                    }
                }
                const nlohmann::json mark = {
                    {"type", "frameMark"},
                    {"seq", videoFrameIndex},
                    {"srvMs", srv_wall},
                    {"capMs", cap_ms},
                    {"encMs", enc_ms},
                };
                const std::string payload = mark.dump();
                for (const auto& id_client : m_clients) {
                    auto ch = id_client.second->get_data_channel();
                    if (!ch) continue;
                    try {
                        ch->send(payload);
                    } catch (...) {
                    }
                }

                // Capture health telemetry for frontend visualization/debug.
                // Keep it low frequency to avoid extra signaling overhead.
                constexpr uint64_t kCaptureHealthInterval = 30;
                if ((videoFrameIndex % kCaptureHealthInterval) == 0) {
                    const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                    const bool forceSwActive = (forceSwUntilMs != 0 && nowMs < forceSwUntilMs);
                    const nlohmann::json health = {
                        {"type", "captureHealth"},
                        {"backend", capHw ? "dxgi" : "gdi"},
                        {"dxgiDisabledForSession", dxgiDisabled},
                        {"topBlackStripStreak", topBlackStripStreak},
                        {"dxgiInstabilityScore", dxgiInstabilityScore},
                        {"forceSoftwareActive", forceSwActive},
                        {"forceSoftwareRemainMs", forceSwActive ? (forceSwUntilMs - nowMs) : 0},
                        {"capMs", cap_ms},
                        {"encMs", enc_ms},
                    };
                    const std::string healthPayload = health.dump();
                    for (const auto& id_client : m_clients) {
                        auto ch = id_client.second->get_data_channel();
                        if (!ch) continue;
                        try {
                            ch->send(healthPayload);
                        } catch (...) {
                        }
                    }
                }
            }
        }
        std::vector<ClientTrack> tracks{};
        std::string streamType = type == Stream::StreamSourceType::Video ? "video" : "audio";

        // get track for given type
        std::function<std::optional<std::shared_ptr<ClientTrackData>>(std::shared_ptr<ClientPeerConnection>)> getTrackData = [type](std::shared_ptr<ClientPeerConnection> client)
            {
                return type == Stream::StreamSourceType::Video ? client->m_video: client->m_audio;
            };
        // get all clients with Ready state
        for (auto id_client : m_clients) {
            auto id = id_client.first;
            auto client = id_client.second;

            auto optTrackData = getTrackData(client);
            if (client->get_state() == ClientPeerConnection::State::Ready && optTrackData.has_value()) {
                auto trackData = optTrackData.value();
                tracks.push_back(ClientTrack(id, trackData));
            }
        }
        if (!tracks.empty()) {
            const bool skipEmptyVideo =
                (type == Stream::StreamSourceType::Video && sample.empty());
            if (!skipEmptyVideo) {
                for (auto clientTrack : tracks) {
                    auto client = clientTrack.id;
                    auto trackData = clientTrack.trackData;

                    //std::cout << "Sending " << streamType << " sample with size: " << std::to_string(sample.size()) << " to " << client << std::endl;
                    try {
                        trackData->track->sendFrame(sample, std::chrono::duration<double, std::micro>(sampleTime));
                    }
                    catch (const std::exception& e) {
                        std::cerr << "Unable to send " << streamType << " packet: " << e.what() << std::endl;
                    }
                }
            }
        }
        m_thread_queue.dispatch([ws, this]() {
            if (m_clients.empty()) {
                // we have no clients, stop the stream
                if (auto stream = ws.lock()) {
                    stream->stop();
                }
            }
            });
        });
    {
        std::scoped_lock lk(m_stream_mtx);
        if (!m_stream) {
            m_stream = stream;
        }
    }
    std::cout << "[stream] _get_or_create_stream done" << std::endl;
    return m_stream;
}

void WebRTCSocket::_broadcast_remote_process_exited()
{
    const nlohmann::json msg = { {"type", "remoteProcessExited"} };
    const std::string payload = msg.dump();
    for (const auto& id_client : m_clients) {
        auto ch = id_client.second->get_data_channel();
        if (!ch) continue;
        try {
            ch->send(payload);
        } catch (...) {
        }
    }
}

void WebRTCSocket::_stop_and_reset_stream()
{
    // Cancel any pending idle-close.
    m_idle_close_gen.fetch_add(1, std::memory_order_relaxed);

    // Notify all active frontends to leave video page before teardown.
    _broadcast_remote_process_exited();

    // Ensure all client PeerConnections are closed and forgotten.
    _close_all_clients();

    InputController::instance()->clear_mouse_target();

    std::shared_ptr<Stream> local;
    {
        std::scoped_lock lk(m_stream_mtx);
        local = m_stream;
        m_stream.reset();
    }
    if (local) {
        try { local->stop(); } catch (...) {}
    }
}

void WebRTCSocket::_close_all_clients()
{
    std::vector<std::shared_ptr<ClientPeerConnection>> clients;
    clients.reserve(m_clients.size());
    for (auto& kv : m_clients) {
        clients.push_back(kv.second);
    }
    m_clients.clear();
    {
        std::scoped_lock lk(m_control_mtx);
        m_controller_id.clear();
    }

    for (auto& c : clients) {
        if (!c) continue;
        try {
            auto pc = c->get_peer_connection();
            if (pc) pc->close();
        } catch (...) {
        }
    }
}

void WebRTCSocket::_schedule_idle_close(int idleSeconds)
{
    const uint64_t gen = m_idle_close_gen.fetch_add(1, std::memory_order_relaxed) + 1;
    const int waitSec = (idleSeconds < 0) ? 0 : idleSeconds;
    std::thread([this, gen, waitSec]() {
        std::this_thread::sleep_for(std::chrono::seconds(waitSec));
        m_thread_queue.dispatch([this, gen]() {
            // If a new schedule happened, or clients re-connected, skip.
            if (m_idle_close_gen.load(std::memory_order_relaxed) != gen) return;
            if (!m_clients.empty()) return;
            _stop_and_reset_stream();
        });
    }).detach();
}

bool WebRTCSocket::_request_control(const std::string& clientId, std::shared_ptr<rtc::DataChannel> dc)
{
    std::scoped_lock lk(m_control_mtx);
    if (m_controller_id.empty() || m_controller_id == clientId) {
        m_controller_id = clientId;
        return true;
    }
    return false;
}

void WebRTCSocket::_release_control(const std::string& clientId)
{
    std::scoped_lock lk(m_control_mtx);
    if (m_controller_id == clientId) {
        m_controller_id.clear();
    }
}

bool WebRTCSocket::_is_controller(const std::string& clientId)
{
    std::scoped_lock lk(m_control_mtx);
    return (!m_controller_id.empty() && m_controller_id == clientId);
}
