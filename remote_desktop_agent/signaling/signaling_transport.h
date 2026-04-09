////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： WebSocket 信令传输（I/O 与 JSON 解析）
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- 仅负责连接信令服务器、收发文本帧、将 JSON 转为 signaling_event
//- 通过 DispatchQueue 将回调收敛到单线程，避免与 libdatachannel 回调线程直接交织
//- 不负责 exe 路径、进程、媒体管线；连接断开时可通知上层做会话清理
//- 对外提供 websocket_weak，供 WebRTC PeerConnection 侧发送 Offer 等信令消息
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "rtc/rtc.hpp"
#include "nlohmann/json.hpp"
#include "transport/dispatch_queue.hpp"
#include "bootstrap/runtime_settings.h"

#include "signaling/signaling_event.h"

#include <functional>
#include <memory>
#include <string>

class signaling_observer;

class signaling_transport {
public:
    explicit signaling_transport(const runtime_settings& settings);

    void set_observer(signaling_observer* observer) { m_observer = observer; }

    void start(const std::string& ip, int port);
    void send_json_text(const std::string& json_text);
    void set_on_transport_closed(std::function<void()> callback) { m_on_transport_closed = std::move(callback); }

    std::weak_ptr<rtc::WebSocket> websocket_weak() const { return m_websocket; }

private:
    void dispatch_incoming_message(const std::string& json_text);
    signaling_event parse_message(const nlohmann::json& message) const;

    runtime_settings m_settings;
    DispatchQueue m_thread_queue;
    std::shared_ptr<rtc::WebSocket> m_websocket;
    std::string m_signaling_url;
    signaling_observer* m_observer = nullptr;
    std::function<void()> m_on_transport_closed;
};
