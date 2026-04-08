#include "transport/client_peer_connection.h"
#include "nlohmann/json.hpp"
#include "input/input_controller.h"
#include "session/remote_process_stream_source.h"
#include <chrono>
#include <unordered_map>


ClientTrackData::ClientTrackData(std::shared_ptr<rtc::Track> track, std::shared_ptr<rtc::RtcpSrReporter> sender)
{
	this->track = track;
	this->sender = sender;
}

ClientTrack::ClientTrack(std::string id, std::shared_ptr<ClientTrackData> trackData)
{
	this->id = id;
	this->trackData = trackData;
}


ClientPeerConnection::ClientPeerConnection(std::shared_ptr<rtc::PeerConnection> pc, std::weak_ptr<rtc::WebSocket> ws, std::string id,
                                           request_control_callback requestControl,
                                           release_control_callback releaseControl,
                                           is_controller_callback isController,
                                           std::shared_ptr<FileTransferService> file_transfer_service,
                                           bool enable_media_tracks)
    : m_peer_connection(pc)
    , m_state(State::Waiting)
    , m_id(std::move(id))
    , m_thread_queue("ClientPeerConnection")
    , m_request_control_callback(std::move(requestControl))
    , m_release_control_callback(std::move(releaseControl))
    , m_is_controller_callback(std::move(isController))
    , m_file_transfer_service(std::move(file_transfer_service))
    , m_enable_control(enable_media_tracks)
{
    if (enable_media_tracks) {
        _add_video(102, 1, "video-stream", "stream1", [this]()
             {
                 m_thread_queue.dispatch([this](){
                     std::cout << "[track] video open for client=" << m_id << std::endl;
                     _add_to_stream(true);
                    });

            });

        _add_audio(111, 2, "audio-stream", "stream1", [this]()
            {
                m_thread_queue.dispatch([this]() {
                    std::cout << "[track] audio open for client=" << m_id << std::endl;
                    _add_to_stream(false);
                    });
            });
    }

    auto dc = m_peer_connection->createDataChannel("ping-pong");
    const std::string clientId = m_id;
    // 数据通道打开后尽快尝试授予控制权并发送 controlGranted。
    // 这样可以避免仅依赖前端 controlRequest 时的竞态。

    dc->onOpen([clientId, wdc = make_weak_ptr(dc), this]() {
        if (auto ch = wdc.lock()) {
            ch->send("Ping");
            if (m_enable_control && m_request_control_callback) {
                const bool ok = m_request_control_callback(clientId, ch);
                if (ok) {
                    ch->send(nlohmann::json({ {"type", "controlGranted"} }).dump());
                } else {
                    ch->send(nlohmann::json({ {"type", "controlDenied"} }).dump());
                }
            }
        }
        });

    dc->onMessage(nullptr, [clientId, wdc = make_weak_ptr(dc), this](std::string msg) {
        if (auto dc = wdc.lock()) {
            _on_data_channel_message(clientId, dc, msg);
        }
    });
    {
        std::unique_lock lock(m_mutex);
        m_data_channel = dc;
    }
}

ClientPeerConnection::~ClientPeerConnection()
{
    release_mouse_target_binding();
}

void ClientPeerConnection::release_mouse_target_binding()
{
    InputController::instance()->unbind_mouse_target();
}

bool ClientPeerConnection::_is_pong_message(const std::string& msg) const
{
    // 仅处理纯文本心跳 Pong，避免误匹配文件分片 JSON/base64 内容中的 "Pong" 子串。
    return (msg == "Pong" || msg.rfind("Pong ", 0) == 0);
}

void ClientPeerConnection::_on_data_channel_message(const std::string& clientId, const std::shared_ptr<rtc::DataChannel>& dc, const std::string& msg)
{
    try {
        if (_is_pong_message(msg)) {
            dc->send("Ping"); // 仅为 Pong 响应，直接忽略
            return;
        }

        auto json = nlohmann::json::parse(msg);
        const std::string type = json.value("type", "");

        if (m_file_transfer_service && m_file_transfer_service->can_handle_type(type)) {
            // 文件传输涉及 base64 编解码 + 文件 IO，避免阻塞 DataChannel 回调线程。
            auto payload = std::move(json);
            m_thread_queue.dispatch([this, clientId, payload = std::move(payload), dc]() mutable {
                if (m_file_transfer_service) {
                    m_file_transfer_service->handle_message(clientId, payload, dc);
                }
            });
            return;
        }

        if (type == "latPing") {
            _handle_lat_ping(json, dc);
            return;
        }
        if (type == "controlRequest") {
            _handle_control_request(clientId, dc);
            return;
        }
        if (type == "controlRelease") {
            _handle_control_release(clientId, dc);
            return;
        }
        if (type == "requestKeyframe") {
            _handle_request_keyframe(clientId);
            return;
        }

        // 忽略非控制者输入
        if (m_is_controller_callback && !m_is_controller_callback(clientId)) {
            return;
        }

        _handle_input_by_type(type, json);
    }
    catch (const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
    }
}

void ClientPeerConnection::_handle_lat_ping(const nlohmann::json& json, const std::shared_ptr<rtc::DataChannel>& dc)
{
    const int64_t srv = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    dc->send(nlohmann::json{
        {"type", "latPong"},
        {"seq", json.value("seq", 0)},
        {"tCli", json.value("tCli", 0LL)},
        {"tSrv", srv}
    }.dump());
}

void ClientPeerConnection::_handle_control_request(const std::string& clientId, const std::shared_ptr<rtc::DataChannel>& dc)
{
    if (!m_enable_control) {
        dc->send(nlohmann::json({ {"type","controlDenied"} }).dump());
        return;
    }
    const bool ok = m_request_control_callback ? m_request_control_callback(clientId, dc) : false;
    dc->send(nlohmann::json({ {"type", ok ? "controlGranted" : "controlDenied"} }).dump());
}

void ClientPeerConnection::_handle_control_release(const std::string& clientId, const std::shared_ptr<rtc::DataChannel>& dc)
{
    if (m_release_control_callback) m_release_control_callback(clientId);
    dc->send(nlohmann::json({ {"type","controlRevoked"} }).dump());
}

void ClientPeerConnection::_handle_request_keyframe(const std::string& clientId)
{
    // 接收端在持续丢包/抖动后请求关键帧。
    // 将操作下沉到 ClientPeerConnection 的工作队列，避免与 on_sample/媒体线程并发读写 m_av_stream。
    m_thread_queue.dispatch([this, clientId]() {
        try {
            if (m_av_stream.has_value()) {
                auto pm = std::dynamic_pointer_cast<RemoteProcessStreamSource>(m_av_stream.value()->m_c_video);
                if (pm) {
                    std::cout << "[keyframe] request_force_keyframe from client=" << clientId << std::endl;
                    pm->request_force_keyframe();
                }
            }
        } catch (...) {}
    });
}

void ClientPeerConnection::_handle_input_by_type(const std::string& type, const nlohmann::json& json)
{
    using Handler = void (ClientPeerConnection::*)(const nlohmann::json&);
    static const std::unordered_map<std::string, Handler> handlers = {
        {"mouseMove", &ClientPeerConnection::_handle_mouse_move},
        {"mouseDown", &ClientPeerConnection::_handle_mouse_down},
        {"mouseUp", &ClientPeerConnection::_handle_mouse_up},
        {"mouseDoubleClick", &ClientPeerConnection::_handle_mouse_double_click},
        {"mouseWheel", &ClientPeerConnection::_handle_mouse_wheel},
        {"keyDown", &ClientPeerConnection::_handle_key_down},
        {"keyUp", &ClientPeerConnection::_handle_key_up},
    };

    const auto it = handlers.find(type);
    if (it == handlers.end()) return;
    (this->*(it->second))(json);
}

void ClientPeerConnection::set_state(State state)
{
	std::unique_lock lock(m_mutex);
	this->m_state = state;
}

ClientPeerConnection::State ClientPeerConnection::get_state()
{
	std::shared_lock lock(m_mutex);
	return m_state;
}

std::shared_ptr<rtc::DataChannel> ClientPeerConnection::get_data_channel() const
{
    std::shared_lock lock(m_mutex);
    if (!m_data_channel.has_value()) return nullptr;
    return m_data_channel.value();
}

std::optional<std::shared_ptr<ClientTrackData>> ClientPeerConnection::get_video_track_data() const
{
    std::shared_lock lock(m_mutex);
    return m_video;
}

std::optional<std::shared_ptr<ClientTrackData>> ClientPeerConnection::get_audio_track_data() const
{
    std::shared_lock lock(m_mutex);
    return m_audio;
}

void ClientPeerConnection::set_callback(create_stream_callback cb)
{
    m_create_stream_callback = cb;
}

void ClientPeerConnection::_add_video(const uint8_t payloadType, const uint32_t ssrc, const std::string cname, const std::string msid, const std::function<void(void)> onOpen)
{
    auto video = rtc::Description::Video(cname);
    video.addH264Codec(payloadType);
    video.addSSRC(ssrc, cname, msid, cname);
    auto track = m_peer_connection->addTrack(video);

    // 创建 RTP 配置
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, payloadType, rtc::H264RtpPacketizer::ClockRate);
    // 创建打包器
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::Length, rtpConfig);
    // 添加 RTCP SR 处理器
    auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    // 添加 RTCP NACK 处理器
    auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(nackResponder);
    // 设置处理链
    track->setMediaHandler(packetizer);

    track->onOpen(onOpen);

    {
        std::unique_lock lock(m_mutex);
        m_video = std::make_shared<ClientTrackData>(track, srReporter);
    }
}

void ClientPeerConnection::_add_audio(const uint8_t payloadType, const uint32_t ssrc, const std::string cname, const std::string msid, const std::function<void(void)> onOpen)
{
    auto audio = rtc::Description::Audio(cname);
    audio.addOpusCodec(payloadType);
    audio.addSSRC(ssrc, cname, msid, cname);
    auto track = m_peer_connection->addTrack(audio);

    // 创建 RTP 配置
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, payloadType, rtc::OpusRtpPacketizer::DefaultClockRate);
    // 创建打包器
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
    // 添加 RTCP SR 处理器
    auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    // 添加 RTCP NACK 处理器
    auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(nackResponder);
    // 设置处理链
    track->setMediaHandler(packetizer);

    track->onOpen(onOpen);

    {
        std::unique_lock lock(m_mutex);
        m_audio = std::make_shared<ClientTrackData>(track, srReporter);
    }
}

void ClientPeerConnection::_add_to_stream(bool is_adding_video)
{
    std::cout << "[track] add_to_stream client=" << m_id
              << " isVideo=" << (is_adding_video ? 1 : 0)
              << " state=" << (int)get_state() << std::endl;
    if (get_state() == ClientPeerConnection::State::Waiting) {
        set_state(is_adding_video ? ClientPeerConnection::State::WaitingForAudio : ClientPeerConnection::State::WaitingForVideo);
    }
    else if ((get_state() == ClientPeerConnection::State::WaitingForAudio && !is_adding_video)
        || (get_state() == ClientPeerConnection::State::WaitingForVideo && is_adding_video)) 
    {
        // 音视频轨道已就绪
        {
            std::shared_lock lk(m_mutex);
            assert(m_video.has_value() && m_audio.has_value());
        }

        set_state(ClientPeerConnection::State::Ready);
        std::cout << "[track] client=" << m_id << " state -> Ready" << std::endl;

        // 就绪后立即启动流（避免依赖第二次状态读取）。
        _start_stream();
        return;
    }

    if (get_state() == ClientPeerConnection::State::Ready)
        _start_stream();
}

void ClientPeerConnection::_bind_mouse_target_for_stream(const std::shared_ptr<Stream>& stream)
{
    if (!stream) return;
    if (auto pm = std::dynamic_pointer_cast<RemoteProcessStreamSource>(stream->m_c_video)) {
        InputController::instance()->bind_mouse_target(pm->get_main_window());
    }
}

void ClientPeerConnection::_start_stream()
{
    std::cout << "[stream] _start_stream enter client=" << m_id << std::endl;
    std::shared_ptr<Stream> stream;
    if (m_av_stream.has_value()) {
        stream = m_av_stream.value();
        if (stream->m_c_is_running) {
            // 流已在运行
            std::cout << "[stream] already running client=" << m_id << std::endl;
            _bind_mouse_target_for_stream(stream);
            return;
        }
    }
    else {
        if (!m_create_stream_callback) {
            std::cout << "[stream] create_stream_callback not set client=" << m_id << std::endl;
            return;
        }
        try {
            stream = m_create_stream_callback();
        } catch (const std::exception& e) {
            std::cout << "[stream] create_stream_callback exception client=" << m_id << " err=" << e.what() << std::endl;
            return;
        } catch (...) {
            std::cout << "[stream] create_stream_callback unknown exception client=" << m_id << std::endl;
            return;
        }
        if (!stream) {
            std::cout << "[stream] create_stream_callback returned null client=" << m_id << std::endl;
            return;
        }
        m_av_stream = stream;
    }
    std::cout << "[stream] start for client=" << m_id << std::endl;
    _bind_mouse_target_for_stream(stream);
    stream->start();
}

void ClientPeerConnection::_handle_mouse_move(const nlohmann::json& json)
{
    int x = json.value("x", 0);
    int y = json.value("y", 0);
    int absX = json.value("absoluteX", 0);
    int absY = json.value("absoluteY", 0);
    int videoWidth = json.value("videoWidth", 0);
    int videoHeight = json.value("videoHeight", 0);
    InputController::instance()->simulate_mouse_move(x, y, absX, absY, videoWidth, videoHeight);

}

void ClientPeerConnection::_handle_mouse_down(const nlohmann::json& json)
{
    int button = json.value("button", 0);
    int x = json.value("x", 0);
    int y = json.value("y", 0);
    InputController::instance()->simulate_mouse_down(button, x, y);
}

void ClientPeerConnection::_handle_mouse_up(const nlohmann::json& json)
{
	int button = json.value("button", 0);
	int x = json.value("x", 0);
	int y = json.value("y", 0);
	InputController::instance()->simulate_mouse_up(button, x, y);
}

void ClientPeerConnection::_handle_mouse_double_click(const nlohmann::json& json)
{
	int button = json.value("button", 0);
	int x = json.value("x", 0);
	int y = json.value("y", 0);
	InputController::instance()->simulate_mouse_double_click(button, x, y);
}

void ClientPeerConnection::_handle_mouse_wheel(const nlohmann::json& json)
{
	int deltaX = json.value("deltaX", 0);
	int deltaY = json.value("deltaY", 0);
	int x = json.value("x", 0);
	int y = json.value("y", 0);
	InputController::instance()->simulate_mouse_wheel(deltaX, deltaY, x, y);
}

static int json_get_windows_vk(const nlohmann::json& json)
{
	// 前端 "key"/"code" 字段是字符串，避免使用 value("key", 0) 导致 type_error 丢包。
	if (json.contains("vk")) {
		try {
			const auto& jv = json["vk"];
			if (jv.is_number_integer() || jv.is_number_unsigned())
				return jv.get<int>();
			if (jv.is_number_float())
				return static_cast<int>(jv.get<double>());
		} catch (...) {
		}
	}
	try {
		return json.value("keyCode", 0);
	} catch (...) {
		return 0;
	}
}

void ClientPeerConnection::_handle_key_down(const nlohmann::json& json)
{
	const int vk = json_get_windows_vk(json);
	const int shiftKey = json.value("shiftKey", 0);
	const int ctrlKey = json.value("ctrlKey", 0);
	const int altKey = json.value("altKey", 0);
	const int metaKey = json.value("metaKey", 0);
	InputController::instance()->simulate_key_down(0, 0, vk, shiftKey, ctrlKey, altKey, metaKey);
}

void ClientPeerConnection::_handle_key_up(const nlohmann::json& json)
{
	const int vk = json_get_windows_vk(json);
	const int shiftKey = json.value("shiftKey", 0);
	const int ctrlKey = json.value("ctrlKey", 0);
	const int altKey = json.value("altKey", 0);
	const int metaKey = json.value("metaKey", 0);
	InputController::instance()->simulate_key_up(0, 0, vk, shiftKey, ctrlKey, altKey, metaKey);
}

const std::shared_ptr<rtc::PeerConnection>& ClientPeerConnection::get_peer_connection()
{
    return m_peer_connection;
}
