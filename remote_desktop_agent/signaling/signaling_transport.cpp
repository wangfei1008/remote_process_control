#include "signaling/signaling_transport.h"
#include "signaling/signaling_observer.h"

#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          构造信令传输对象，初始化内部任务队列（用于串行处理 WebSocket 回调）
/// @参数
///          settings--运行时配置（含 signaling_local_id 等）
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
signaling_transport::signaling_transport(const runtime_settings& settings)
    : m_settings(settings)
    , m_thread_queue("signaling_transport")
{
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          建立 WebSocket 连接，注册收发回调；阻塞等待连接成功或失败返回
/// @参数
///          ip--信令服务地址
///          port--信令服务端口
/// @返回值
///          无（连接失败时打印日志并返回）
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void signaling_transport::start(const std::string& ip, int port)
{
    m_websocket = std::make_shared<rtc::WebSocket>();

    m_websocket->onOpen([] {});
    m_websocket->onClosed([this]() {
        if (m_on_transport_closed) m_thread_queue.dispatch(m_on_transport_closed);
    });
    m_websocket->onError([](const std::string&) {});
    m_websocket->onMessage([this](const std::variant<rtc::binary, std::string>& data) {
        if (!std::holds_alternative<std::string>(data)) return;
        const std::string& text = std::get<std::string>(data);
        m_thread_queue.dispatch([this, text]() { dispatch_incoming_message(text); });
    });

    m_signaling_url = "ws://" + ip + ":" + std::to_string(port) + "/" + m_settings.signaling_local_id;
    std::cout << "[signaling] opening " << m_signaling_url << std::endl;
    try {
        m_websocket->open(m_signaling_url);
    } catch (const std::exception& e) {
        std::cerr << "[signaling] open failed: " << e.what() << std::endl;
        return;
    } catch (...) {
        std::cerr << "[signaling] open failed: unknown" << std::endl;
        return;
    }

    while (!m_websocket->isOpen()) {
        if (m_websocket->isClosed()) return;
        std::this_thread::sleep_for(100ms);
    }
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          在 WebSocket 已打开时发送一条文本帧（通常为 JSON 信令）
/// @参数
///          json_text--待发送的 UTF-8 文本
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void signaling_transport::send_json_text(const std::string& json_text)
{
    if (!m_websocket || !m_websocket->isOpen()) return;
    try {
        m_websocket->send(json_text);
    } catch (...) {
    }
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          解析 JSON 并转换为 signaling_event，投递给已注册的 Observer
/// @参数
///          json_text--对端发来的信令文本
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void signaling_transport::dispatch_incoming_message(const std::string& json_text)
{
    if (!m_observer) return;
    try {
        nlohmann::json j = nlohmann::json::parse(json_text);
        signaling_event ev = parse_message(j);
        m_observer->on_signaling_event(ev);
    } catch (...) {
        std::cerr << "[signaling] invalid json" << std::endl;
    }
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          将 nlohmann::json 消息映射为领域事件（request/file_request/answer/stop）
/// @参数
///          message--已解析的 JSON 对象
/// @返回值
///          填充后的 signaling_event；无法识别时 type 为 invalid
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
signaling_event signaling_transport::parse_message(const nlohmann::json& message) const
{
    signaling_event ev;
    auto it = message.find("id");
    if (it == message.end() || !it->is_string()) return ev;
    ev.client_id = it->get<std::string>();
    it = message.find("type");
    if (it == message.end() || !it->is_string()) return ev;
    const std::string type = it->get<std::string>();
    if (type == "request") {
        ev.type = signaling_event_type::media_session_requested;
        ev.exe_path = message.value("exePath", "");
        return ev;
    }
    if (type == "file_request") {
        ev.type = signaling_event_type::file_only_session_requested;
        return ev;
    }
    if (type == "answer") {
        ev.type = signaling_event_type::sdp_answer;
        if (message.contains("sdp") && message["sdp"].is_string())
            ev.sdp_text = message["sdp"].get<std::string>();
        return ev;
    }
    if (type == "stop") {
        ev.type = signaling_event_type::stop_session;
        return ev;
    }
    return ev;
}
