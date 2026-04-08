////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 单一操作者 WebRTC 通道（PeerConnection + ClientPeerConnection）
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- 封装一条rtc::PeerConnection：ICE 收集完成后通过回调发送 Offer JSON
//- 内部持有 ClientPeerConnection：音视频轨、DataChannel、键鼠与文件消息（复用现有实现）
//- 连接断开时通过 DispatchQueue 通知上层，便于 session_director 释放会话
//- 单人场景下控制权限回调恒为授予，无多客户端仲裁
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "rtc/rtc.hpp"
#include "transport/client_peer_connection.h"
#include "transport/remote_file_transfer_controller.h"
#include "transport/dispatch_queue.hpp"

#include <functional>
#include <memory>
#include <string>

class remote_desktop_media_session;

class operator_channel {
public:
    using create_media_session_callback = std::function<std::shared_ptr<remote_desktop_media_session>()>;
    using send_signaling_fn = std::function<void(const std::string& json_text)>;
    using connection_lost_fn = std::function<void()>;

    operator_channel(rtc::Configuration config,
                     std::weak_ptr<rtc::WebSocket> websocket,
                     std::string client_id,
                     std::shared_ptr<remote_file_transfer_controller> file_service,
                     bool enable_media_tracks,
                     create_media_session_callback create_media_session,
                     send_signaling_fn send_offer,
                     DispatchQueue& io_dispatch,
                     connection_lost_fn on_connection_lost);

    ~operator_channel();

    std::shared_ptr<rtc::PeerConnection> peer_connection() const { return m_peer_connection; }
    std::shared_ptr<ClientPeerConnection> client_peer() const { return m_client_peer; }

    void start_local_negotiation();
    void set_remote_answer(const std::string& sdp_text);
    void close();

private:
    rtc::Configuration m_config;
    std::weak_ptr<rtc::WebSocket> m_websocket;
    std::string m_client_id;
    std::shared_ptr<rtc::PeerConnection> m_peer_connection;
    std::shared_ptr<ClientPeerConnection> m_client_peer;
    send_signaling_fn m_send_signaling;
    DispatchQueue& m_io_dispatch;
    connection_lost_fn m_on_connection_lost;
};
