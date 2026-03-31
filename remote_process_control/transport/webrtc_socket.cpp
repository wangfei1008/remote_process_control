#include "transport/webrtc_socket.h"
#include <thread>
#include <chrono>
#include "input/input_controller.h"
#include "session/remote_process_stream_source.h"
#include "source/silence_opus_source.h"

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
    // 可选 STUN：腾讯 stun.l.tencent.com:3478
    // 可选 STUN：阿里 stun.aliyun.com:3478
    // 可选 STUN：Google stun.l.google.com:19302
    std::string stun_server = "stun.l.google.com:19302";

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
    if (type == "request") // 启动会话请求
    {
        // 每次请求都从干净状态开始，确保是全新会话。
        _stop_and_reset_stream();

        // 前端可选传入进程路径
        if (message.contains("exePath")) {
            const std::string exePath = message.value("exePath", "");
            if (!exePath.empty()) {
                std::scoped_lock lk(m_stream_mtx);
                m_exe_path = exePath;
                // 重置流，确保下次启动使用新进程
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
        client->set_callback([this]() {
            return _get_or_create_stream();
        });

        m_clients.emplace(id, client);
        pc->setLocalDescription();
    }
    else if (type == "answer") // SDP 应答
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
        // 无论由谁触发停止，都执行完整清理，并让前端退出视频页。
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
                // 移除断开的客户端
                m_thread_queue.dispatch([id, this]() {
                    m_clients.erase(id);
                    _release_control(id);
                    // 强生命周期保证：
                    // 当前端关闭页面/流且无客户端时，
                    // 立即释放进程与流资源。
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
                    // 候选收集完成，发送应答
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

    auto video = std::make_shared<RemoteProcessStreamSource>();
    video->set_on_remote_exit([this]() {
        m_thread_queue.dispatch([this]() {
            _stop_and_reset_stream();
        });
    });
    video->launch_process(exePath);
    auto audio = std::make_shared<SilenceOpusSource>();

    auto stream = std::make_shared<Stream>(video, audio);
    // 设置样本发送回调
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
                    if (auto pm = std::dynamic_pointer_cast<RemoteProcessStreamSource>(s->m_c_video)) {
                        std::cout << "[latency][sender] capture_ms=" << pm->get_last_capture_ms()
                                  << " encode_ms=" << pm->get_last_encode_ms()
                                  << " frame_unix_ms=" << pm->get_last_frame_unix_ms() << std::endl;
                    }
                }
            }

            // 降低 frameMark 发送频率，减少 DataChannel/JSON 开销。
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
                    if (auto pm = std::dynamic_pointer_cast<RemoteProcessStreamSource>(s->m_c_video)) {
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

                // 发送采集健康遥测，供前端可视化与调试。
                // 保持低频，避免额外信令开销。
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

        // 获取当前类型对应的轨道
        std::function<std::optional<std::shared_ptr<ClientTrackData>>(std::shared_ptr<ClientPeerConnection>)> getTrackData = [type](std::shared_ptr<ClientPeerConnection> client)
            {
                return type == Stream::StreamSourceType::Video ? client->m_video: client->m_audio;
            };
        // 获取所有 Ready 状态客户端
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
                // 没有客户端时停止流
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
    // 拆除前先通知所有活跃前端离开视频页面。
    _broadcast_remote_process_exited();

    // 确保所有客户端 PeerConnection 都被关闭并清理。
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

bool WebRTCSocket::_request_control(const std::string& clientId, std::shared_ptr<rtc::DataChannel>)
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
