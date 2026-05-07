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

    m_websocket->onOpen([this] {
        std::cout << "[signaling] websocket opened " << m_signaling_url << std::endl;
        try_send_node_register();
    });
    m_websocket->onClosed([this]() {
        std::cout << "[signaling] websocket closed " << m_signaling_url << std::endl;
        if (m_on_transport_closed) m_thread_queue.dispatch(m_on_transport_closed);
    });
    m_websocket->onError([this](const std::string& err) {
        std::cerr << "[signaling] websocket error " << m_signaling_url << " err=" << err << std::endl;
    });
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
    } catch (const std::exception& e) {
        std::cerr << "[signaling] send failed: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[signaling] send failed: unknown" << std::endl;
    }
}

void signaling_transport::try_send_node_register()
{
    if (m_settings.node_auth_key.empty()) {
        std::cout << "[signaling] 未设置 RPC_NODE_AUTH_KEY，跳过 node_register（与受管信令库联调时请配置节点密钥）" << std::endl;
        return;
    }
    nlohmann::json out;
    out["type"] = "node_register";
    out["data"]["node_name"] = m_settings.signaling_local_id;
    out["data"]["auth_key"] = m_settings.node_auth_key;
    try {
        send_json_text(out.dump());
        std::cout << "[signaling] 已发送 node_register node_name=" << m_settings.signaling_local_id << std::endl;
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
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[signaling] json parse error: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[signaling] invalid message: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[signaling] invalid message" << std::endl;
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
    auto type_it = message.find("type");
    if (type_it == message.end() || !type_it->is_string())
        return ev;
    const std::string type = type_it->get<std::string>();

    // 受管模式：信令服务主动下发（无顶层 id，与《接口》agent_start_session 一致）
    if (type == "agent_start_session") {
        ev.type = signaling_event_type::agent_start_session;
        if (!message.contains("data") || !message["data"].is_object())
            return ev;
        const nlohmann::json& d = message["data"];
        ev.exe_path = d.value("exe_path", std::string());
        ev.signaling_session_id = d.value("session_id", std::string());
        ev.grace_period_sec = d.value("grace_period", 0);
        ev.client_id = d.value("operator_client_id", std::string());
        return ev;
    }

    auto id_it = message.find("id");
    if (id_it == message.end() || !id_it->is_string())
        return ev;
    ev.client_id = id_it->get<std::string>();

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
