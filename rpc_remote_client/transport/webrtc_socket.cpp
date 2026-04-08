#include "transport/webrtc_socket.h"
#include "transport/rpc_media_session.h"
#include "source/stream.h"
#include <thread>
#include <chrono>
#include "common/runtime_config.h"

using namespace std::chrono_literals;

WebRTCSocket::WebRTCSocket() 
    : m_ws(nullptr)
    , m_signaling_ip("")
    , m_signaling_url("")
    , m_thread_queue("WebRTCSocket")
    , m_media_session(std::make_unique<RpcMediaSession>())
{
	rtc::InitLogger(rtc::LogLevel::Info);
    m_stun_server = runtime_config::get_string("RPC_WEBRTC_STUN_SERVER", "stun.l.google.com:19302");
    m_signaling_local_id = runtime_config::get_string("RPC_SIGNALING_LOCAL_ID", "server");
}

WebRTCSocket::~WebRTCSocket() = default;

void WebRTCSocket::start(const std::string& ip, int port)
{
	m_signaling_ip = ip;
    m_signaling_port = port;
	_init_signaling();
}

void WebRTCSocket::_init_signaling()
{    
    // 可选 STUN：腾讯 stun.l.tencent.com:3478
    // 可选 STUN：阿里 stun.aliyun.com:3478
    // 可选 STUN：Google stun.l.google.com:19302
    m_config.iceServers.emplace_back(m_stun_server);
    m_config.disableAutoNegotiation = true;

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

    m_signaling_url = "ws://" + m_signaling_ip + ":" + std::to_string(m_signaling_port) + "/" + m_signaling_local_id;
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
    if (type == "request" || type == "file_request") // 启动会话请求
    {
        const bool media_enabled = (type == "request");
        // 每次请求都从干净状态开始，确保是全新会话。
        if (media_enabled) {
            _stop_and_reset_stream();
        } else {
            std::shared_ptr<ClientPeerConnection> old_client;
            {
                std::unique_lock<std::shared_mutex> lk(m_clients_mtx);
                auto it_client = m_clients.find(id);
                if (it_client != m_clients.end()) {
                    old_client = it_client->second;
                    m_clients.erase(it_client);
                }
            }
            if (old_client) {
                try {
                    if (auto old_pc = old_client->get_peer_connection()) {
                        old_pc->close();
                    }
                } catch (...) {
                }
                _release_control(id);
            }
        }

        // 进程路径由前端请求提供；未提供则不启动远程进程、不发送视频。
        if (media_enabled) {
            const std::string exePath = message.value("exePath", "");
            m_media_session->reset_for_new_media_request(exePath);
        }

        auto pc = _init_peer_connection(id);
        auto client = std::make_shared<ClientPeerConnection>(
            pc, make_weak_ptr(m_ws), id,
            [this](const std::string& clientId, std::shared_ptr<rtc::DataChannel> dc) { return _request_control(clientId, dc); },
            [this](const std::string& clientId) { _release_control(clientId); },
            [this](const std::string& clientId) { return _is_controller(clientId); },
            m_media_session->file_transfer_service(),
            media_enabled
        );
        if (media_enabled) {
            client->set_callback([this]() {
                return _get_or_create_stream();
            });
        }

        {
            std::unique_lock<std::shared_mutex> lk(m_clients_mtx);
            m_clients.emplace(id, client);
        }
        pc->setLocalDescription();
    }
    else if (type == "answer") // SDP 应答
    {
        std::shared_ptr<ClientPeerConnection> client;
        {
            std::shared_lock<std::shared_mutex> lk(m_clients_mtx);
            auto jt = m_clients.find(id);
            if (jt != m_clients.end()) client = jt->second;
        }
        if (client) {
            auto pc = client->get_peer_connection();
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
                    {
                        std::unique_lock<std::shared_mutex> lk(m_clients_mtx);
                        m_clients.erase(id);
                    }
                    _release_control(id);
                    // 强生命周期保证：
                    // 当前端关闭页面/流且无客户端时，
                    // 立即释放进程与流资源。
                    bool empty = false;
                    {
                        std::shared_lock<std::shared_mutex> lk(m_clients_mtx);
                        empty = m_clients.empty();
                    }
                    if (empty) {
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

std::vector<std::shared_ptr<ClientPeerConnection>> WebRTCSocket::_snapshot_clients() const
{
    std::vector<std::shared_ptr<ClientPeerConnection>> clients;
    std::shared_lock<std::shared_mutex> lk(m_clients_mtx);
    clients.reserve(m_clients.size());
    for (const auto& kv : m_clients) clients.push_back(kv.second);
    return clients;
}

std::shared_ptr<Stream> WebRTCSocket::_get_or_create_stream()
{
    return m_media_session->get_or_create_stream(
        [this]() { return _snapshot_clients(); },
        [this]() {
            m_thread_queue.dispatch([this]() { _stop_stream_keep_clients(); });
        },
        [this]() {
            m_thread_queue.dispatch([this]() {
                bool empty = false;
                {
                    std::shared_lock<std::shared_mutex> lk(m_clients_mtx);
                    empty = m_clients.empty();
                }
                if (empty) m_media_session->stop_stream();
            });
        });
}

void WebRTCSocket::_broadcast_remote_process_exited()
{
    const nlohmann::json msg = { {"type", "remoteProcessExited"} };
    const std::string payload = msg.dump();
    std::vector<std::shared_ptr<ClientPeerConnection>> clients;
    {
        std::shared_lock<std::shared_mutex> lk(m_clients_mtx);
        clients.reserve(m_clients.size());
        for (const auto& kv : m_clients) clients.push_back(kv.second);
    }
    for (const auto& client : clients) {
        if (!client) continue;
        auto ch = client->get_data_channel();
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

    m_media_session->stop_stream();
}

void WebRTCSocket::_stop_stream_keep_clients()
{
    // 目标进程结束时，只通知前端退出视频页；保持 signaling 与客户端连接不被强拆。
    _broadcast_remote_process_exited();
    for (const auto& c : _snapshot_clients()) {
        if (c) c->release_mouse_target_binding();
    }
    m_media_session->stop_stream();
}

void WebRTCSocket::_close_all_clients()
{
    std::vector<std::shared_ptr<ClientPeerConnection>> clients;
    {
        std::unique_lock<std::shared_mutex> lk(m_clients_mtx);
        clients.reserve(m_clients.size());
        for (auto& kv : m_clients) {
            clients.push_back(kv.second);
        }
        m_clients.clear();
    }
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
