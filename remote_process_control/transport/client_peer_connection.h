#pragma once

#include "rtc/rtc.hpp"
#include "nlohmann/json.hpp"

#include <shared_mutex>
#include "transport/dispatch_queue.hpp"
#include "source/stream.h"

typedef struct ClientTrackData 
{
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtcpSrReporter> sender;

    ClientTrackData(std::shared_ptr<rtc::Track> track, std::shared_ptr<rtc::RtcpSrReporter> sender);
}ClientTrackData;

typedef struct ClientTrack 
{
    std::string id;
    std::shared_ptr<ClientTrackData> trackData;
    ClientTrack(std::string id, std::shared_ptr<ClientTrackData> trackData);
}ClientTrack;

class ClientPeerConnection
{
    typedef std::function<std::shared_ptr<Stream>()> create_stream_callback;
    typedef std::function<bool(const std::string&, std::shared_ptr<rtc::DataChannel>)> request_control_callback;
    typedef std::function<void(const std::string&)> release_control_callback;
    typedef std::function<bool(const std::string&)> is_controller_callback;

public:
    enum class State {
        Waiting,
        WaitingForVideo,
        WaitingForAudio,
        Ready
    };

    const std::shared_ptr<rtc::PeerConnection>& get_peer_connection();;
    ClientPeerConnection(std::shared_ptr<rtc::PeerConnection> pc, std::weak_ptr<rtc::WebSocket> ws, std::string id,
                         request_control_callback requestControl,
                         release_control_callback releaseControl,
                         is_controller_callback isController);

    void set_state(State state);
    State get_state();

    //向浏览器发送诊断 JSON 消息（例如 frameMark）
    std::shared_ptr<rtc::DataChannel> get_data_channel() const;

	void set_callback(create_stream_callback cb);
private:
    void _add_video(const uint8_t payloadType, const uint32_t ssrc, const std::string cname, const std::string msid, const std::function<void(void)> onOpen);
    void _add_audio(const uint8_t payloadType, const uint32_t ssrc, const std::string cname, const std::string msid, const std::function<void(void)> onOpen);
    void _add_to_stream(bool is_adding_video);
    void _start_stream();
    void _handle_mouse_move(const nlohmann::json& json);
    void _handle_mouse_down(const nlohmann::json& json);
    void _handle_mouse_up(const nlohmann::json& json);
    void _handle_mouse_double_click(const nlohmann::json& json);
    void _handle_mouse_wheel(const nlohmann::json& json);
    void _handle_key_down(const nlohmann::json& json);
    void _handle_key_up(const nlohmann::json& json);

    uint32_t rtpStartTimestamp = 0;
public:
    std::optional<std::shared_ptr<ClientTrackData>> m_video;
    std::optional<std::shared_ptr<ClientTrackData>> m_audio;

private:
    std::optional<std::shared_ptr<rtc::DataChannel>> m_data_channel;
    std::optional<std::shared_ptr<Stream>> m_av_stream;

    std::shared_mutex m_mutex;
    State m_state;
    std::string m_id;
    std::shared_ptr<rtc::PeerConnection> m_peer_connection;
    DispatchQueue m_thread_queue; 

    create_stream_callback m_create_stream_callback;
    request_control_callback m_request_control_callback;
    release_control_callback m_release_control_callback;
    is_controller_callback m_is_controller_callback;
};


template <class T>
std::weak_ptr<T> make_weak_ptr(std::shared_ptr<T> ptr)
{
    return ptr;
}
