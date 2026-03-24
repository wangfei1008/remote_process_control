#include "client_peer_connection.h"
#include "nlohmann/json.hpp"
#include "input_controller.h"
#include "desktop_screen_source.h"
#include "process_manager.h"
#include <chrono>


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
                                           is_controller_callback isController)
    : m_peer_connection(pc)
    , m_state(State::Waiting)
    , m_id(std::move(id))
    , m_thread_queue("ClientPeerConnection")
    , m_request_control_callback(std::move(requestControl))
    , m_release_control_callback(std::move(releaseControl))
    , m_is_controller_callback(std::move(isController))
{
    auto ptr_ws = this;
    //auto wc = make_weak_ptr(std::shared_ptr<ClientPeerConnection>(this));
    _add_video(102, 1, "video-stream", "stream1", [this]()
         {
             m_thread_queue.dispatch([this](){
                 // if (auto c = ptr_ws.lock());addToStream(c, true);
                 std::cout << "[track] video open for client=" << m_id << std::endl;
				 _add_to_stream(true);
                });

        });

    _add_audio(111, 2, "audio-stream", "stream1", [this]() 
        {
            m_thread_queue.dispatch([this]() {
                //if (auto c = wc.lock()) addToStream(c, false);
                std::cout << "[track] audio open for client=" << m_id << std::endl;
				_add_to_stream(false);
                });
        });

    auto dc = m_peer_connection->createDataChannel("ping-pong");
    const std::string clientId = m_id;
    // 通道一打开即尝试授予控制权并下发 controlGranted，避免仅依赖前端 controlRequest 时出现竞态，
    // 导致 m_controller_id 仍为空、所有鼠标键盘消息被丢弃。

    dc->onOpen([clientId, wdc = make_weak_ptr(dc), this]() {
        if (auto ch = wdc.lock()) {
            ch->send("Ping");
            if (m_request_control_callback) {
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
        if (auto dc = wdc.lock()) 
        {
			std::cout << "DataChannel message from " << clientId << ": " << msg << std::endl;
            try {                
				if (msg.find("Pong") != -1) {
                    dc->send("Ping");// Just a pong response, ignore					
					return;
				}

                auto json = nlohmann::json::parse(msg);
                const std::string type = json.value("type", "");
                if (type == "latPing") {
                    const int64_t srv = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    dc->send(nlohmann::json{
                        {"type", "latPong"},
                        {"seq", json.value("seq", 0)},
                        {"tCli", json.value("tCli", 0LL)},
                        {"tSrv", srv}
                    }.dump());
                    return;
                }
                if (type == "controlRequest") {
                    const bool ok = m_request_control_callback ? m_request_control_callback(clientId, dc) : false;
                    if (ok) {
                        dc->send(nlohmann::json({ {"type","controlGranted"} }).dump());
                    } else {
                        dc->send(nlohmann::json({ {"type","controlDenied"} }).dump());
                    }
                    return;
                }
                if (type == "controlRelease") {
                    if (m_release_control_callback) m_release_control_callback(clientId);
                    dc->send(nlohmann::json({ {"type","controlRevoked"} }).dump());
                    return;
                }

                // ignore input from non-controller
                if (m_is_controller_callback && !m_is_controller_callback(clientId)) {
                    return;
                }

                if (type == "mouseMove") {
                    _handle_mouse_move(json);
                }
                else if (type == "mouseDown") {
                    _handle_mouse_down(json);
                }
                else if (type == "mouseUp") {
                    _handle_mouse_up(json);
                }
                else if (type == "mouseDoubleClick") {
                    _handle_mouse_double_click(json);
                }
                else if (type == "mouseWheel") {
                    _handle_mouse_wheel(json);
                }
                else if (type == "keyDown") {
                    _handle_key_down(json);
                }
                else if (type == "keyUp") {
                    _handle_key_up(json);
                }
            }
            catch (const std::exception& e) {
                std::cerr << "JSON parse error: " << e.what() << std::endl;
            }
        }
        });
    m_data_channel = dc;   
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
    if (!m_data_channel.has_value()) return nullptr;
    return m_data_channel.value();
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

    // create RTP configuration
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, payloadType, rtc::H264RtpPacketizer::ClockRate);
    // create packetizer
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::Length, rtpConfig);
    // add RTCP SR handler
    auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    // add RTCP NACK handler
    auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(nackResponder);
    // set handler
    track->setMediaHandler(packetizer);

    track->onOpen(onOpen);

    m_video = std::make_shared<ClientTrackData>(track, srReporter);
}

void ClientPeerConnection::_add_audio(const uint8_t payloadType, const uint32_t ssrc, const std::string cname, const std::string msid, const std::function<void(void)> onOpen)
{
    auto audio = rtc::Description::Audio(cname);
    audio.addOpusCodec(payloadType);
    audio.addSSRC(ssrc, cname, msid, cname);
    auto track = m_peer_connection->addTrack(audio);

    // create RTP configuration
    auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, payloadType, rtc::OpusRtpPacketizer::DefaultClockRate);
    // create packetizer
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
    // add RTCP SR handler
    auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    // add RTCP NACK handler
    auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(nackResponder);
    // set handler
    track->setMediaHandler(packetizer);

    track->onOpen(onOpen);

    m_audio = std::make_shared<ClientTrackData>(track, srReporter);
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
        // Audio and video tracks are collected now
        assert(m_video.has_value() && m_audio.has_value());
        auto video = m_video.value();

        if (m_av_stream.has_value()) {
            _send_initial_nalus();
        }

        set_state(ClientPeerConnection::State::Ready);
        std::cout << "[track] client=" << m_id << " state -> Ready" << std::endl;

        // Immediately start stream once ready (avoid relying on a second state read).
        _start_stream();
        return;
    }

    if (get_state() == ClientPeerConnection::State::Ready)
        _start_stream();
}

void ClientPeerConnection::_send_initial_nalus()
{
    auto h264 = dynamic_cast<DesktopScreenSource*>(m_av_stream.value()->m_c_video.get());
    if (!h264) {
        return;
    }
    auto initialNalus = h264->initial_nalus();

    // send previous NALU key frame so users don't have to wait to see stream works
    if (!initialNalus.empty()) 
    {
        const double frameDuration_s = double(h264->get_sample_duration_us()) / (1000 * 1000);
        const uint32_t frameTimestampDuration = m_video.value()->sender->rtpConfig->secondsToTimestamp(frameDuration_s);
        m_video.value()->sender->rtpConfig->timestamp = m_video.value()->sender->rtpConfig->startTimestamp - frameTimestampDuration * 2;
        m_video.value()->track->send(rtc::binary(initialNalus.begin(), initialNalus.end()));
        m_video.value()->sender->rtpConfig->timestamp += frameTimestampDuration;
        // Send initial NAL units again to start stream in firefox browser
        m_video.value()->track->send(rtc::binary(initialNalus.begin(), initialNalus.end()));
    }
}

void ClientPeerConnection::_start_stream()
{
    std::cout << "[stream] _start_stream enter client=" << m_id << std::endl;
    std::shared_ptr<Stream> stream;
    if (m_av_stream.has_value()) {
        stream = m_av_stream.value();
        if (stream->m_c_is_running) {
            // stream is already running
            std::cout << "[stream] already running client=" << m_id << std::endl;
            return;
        }
    }
    else {
        if (!m_create_stream_callback) {
            std::cout << "[stream] create_stream_callback not set client=" << m_id << std::endl;
            return;
        }
        try {
            stream = m_create_stream_callback(h264SamplesDirectory, 30, opusSamplesDirectory);
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
    if (auto pm = std::dynamic_pointer_cast<ProcessManager>(stream->m_c_video)) {
        InputController::instance()->set_mouse_target(pm->get_main_window());
    }
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
	// 前端 "key"/"code" 为字符串，不能用 value("key",0) 否则会 type_error 整包丢弃
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