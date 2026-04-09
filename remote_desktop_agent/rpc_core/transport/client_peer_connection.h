#pragma once

#include "rtc/rtc.hpp"
#include "nlohmann/json.hpp"

#include <shared_mutex>
#include <optional>
#include "transport/dispatch_queue.hpp"
#include "transport/remote_file_transfer_controller.h"

class remote_desktop_media_session;

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

class ClientPeerConnection : public std::enable_shared_from_this<ClientPeerConnection>
{
    typedef std::function<std::shared_ptr<remote_desktop_media_session>()> create_media_session_callback;
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
                         is_controller_callback isController,
                         std::shared_ptr<remote_file_transfer_controller> file_transfer_controller,
                         bool enable_media_tracks = true);
    ~ClientPeerConnection();

    void set_state(State state);
    State get_state();

    //向浏览器发送诊断 JSON 消息（例如 frameMark）
    std::shared_ptr<rtc::DataChannel> get_data_channel() const;

    // 线程安全读取轨道对象（用于媒体发送端构建 tracks）。
    std::optional<std::shared_ptr<ClientTrackData>> get_video_track_data() const;
    std::optional<std::shared_ptr<ClientTrackData>> get_audio_track_data() const;

    // 释放本连接对 InputController 鼠标目标的绑定（例如共享流被停止但连接仍保留）。
    void release_mouse_target_binding();

	void set_callback(create_media_session_callback cb);
private:
    void _on_data_channel_message(const std::string& clientId,
                                   const std::shared_ptr<rtc::DataChannel>& dc,
                                   const std::string& msg);
    bool _is_pong_message(const std::string& msg) const;

    void _handle_lat_ping(const nlohmann::json& json, const std::shared_ptr<rtc::DataChannel>& dc);
    void _handle_control_request(const std::string& clientId, const std::shared_ptr<rtc::DataChannel>& dc);
    void _handle_control_release(const std::string& clientId, const std::shared_ptr<rtc::DataChannel>& dc);
    void _handle_request_keyframe(const std::string& clientId);

    void _handle_input_by_type(const std::string& type, const nlohmann::json& json);

    void _add_video(const uint8_t payloadType, const uint32_t ssrc, const std::string cname, const std::string msid, const std::function<void(void)> onOpen);
    void _add_audio(const uint8_t payloadType, const uint32_t ssrc, const std::string cname, const std::string msid, const std::function<void(void)> onOpen);
    void _add_to_stream(bool is_adding_video);
    void _start_media_session();
    void _bind_mouse_target_for_media_session(const std::shared_ptr<remote_desktop_media_session>& media_session);
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
    std::optional<std::shared_ptr<remote_desktop_media_session>> m_media_session;

    mutable std::shared_mutex m_mutex;
    State m_state;
    std::string m_id;
    std::shared_ptr<rtc::PeerConnection> m_peer_connection;
    DispatchQueue m_thread_queue; 

    create_media_session_callback m_create_media_session_callback;
    request_control_callback m_request_control_callback;
    release_control_callback m_release_control_callback;
    is_controller_callback m_is_controller_callback;
    std::shared_ptr<remote_file_transfer_controller> m_file_transfer_controller;
    bool m_enable_control = true;
};


template <class T>
std::weak_ptr<T> make_weak_ptr(std::shared_ptr<T> ptr)
{
    return ptr;
}
