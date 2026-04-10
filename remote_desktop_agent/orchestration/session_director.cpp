#include "orchestration/session_director.h"
#include "orchestration/active_desktop_session.h"

#include <iostream>
#include <thread>

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          构造会话编排器：保存配置与信令引用，初始化 RTC 默认参数与替换策略
/// @参数
///          settings--运行时配置快照
///          transport--信令传输（用于取 WebSocket weak 与发送 JSON）
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
session_director::session_director(runtime_settings settings, signaling_transport& transport)
    : m_settings(std::move(settings))
    , m_transport(transport)
    , m_task_queue("session_director")
    , m_replace_policy(std::make_unique<always_replace_session_policy>())
{
    m_rtc_config.iceServers.emplace_back(m_settings.stun_server);
    m_rtc_config.disableAutoNegotiation = true;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          排队执行当前活动会话的完整拆除（供应用退出或信令层关闭时调用）
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void session_director::stop_all_sessions()
{
    m_task_queue.dispatch([this]() { teardown_active(); });
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          Observer 接口：将信令事件入队，在 worker 线程中分支处理（新建 / Answer / Stop）
/// @参数
///          event--结构化信令事件
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void session_director::on_signaling_event(const signaling_event& event)
{
    m_task_queue.dispatch([this, event]() {
        switch (event.type) {
        case signaling_event_type::invalid:
            break;
        case signaling_event_type::media_session_requested:
            if (m_replace_policy && !m_replace_policy->should_replace_existing_session_on_new_media_request()) return;
            if (m_active_session && !m_current_client_id.empty() && event.client_id == m_current_client_id) {
                cancel_pending_disconnect_teardown();
                desktop_session_create_params params;
                params.settings = &m_settings;
                params.rtc_config = m_rtc_config;
                params.websocket = m_transport.websocket_weak();
                params.client_id = event.client_id;
                params.exe_path = event.exe_path;
                params.media_enabled = true;
                params.io_dispatch = &m_task_queue;
                params.post_to_signaling_thread = [this](std::function<void()> fn) { m_task_queue.dispatch(std::move(fn)); };
                params.send_signaling_json = [this](const std::string& json) { m_transport.send_json_text(json); };
                params.on_connection_lost = [this]() { on_operator_connection_lost(); };
                std::cout << "[session_director] reconnect session client_id=" << event.client_id << std::endl;
                m_active_session->reconnect_operator(params);
            } else {
                replace_with_new_session(event.client_id, event.exe_path, true);
            }
            break;
        case signaling_event_type::file_only_session_requested:
            if (m_replace_policy && !m_replace_policy->should_replace_existing_session_on_new_media_request()) return;
            replace_with_new_session(event.client_id, std::string(), false);
            break;
        case signaling_event_type::sdp_answer:
            if (!m_active_session || event.client_id != m_current_client_id) return;
            m_active_session->apply_remote_answer(event.sdp_text);
            break;
        case signaling_event_type::stop_session:
            teardown_active();
            break;
        }
    });
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          先 teardown 当前会话，再按参数创建新 active_desktop_session（新请求替换）
/// @参数
///          client_id--信令侧会话/浏览器标识
///          exe_path--被拉起的远程进程路径；非媒体会话可为空
///          media_enabled--是否创建音视频轨与媒体管线
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void session_director::replace_with_new_session(const std::string& client_id, const std::string& exe_path, bool media_enabled)
{
    teardown_active();
    m_current_client_id = client_id;
    cancel_pending_disconnect_teardown();

    desktop_session_create_params params;
    params.settings = &m_settings;
    params.rtc_config = m_rtc_config;
    params.websocket = m_transport.websocket_weak();
    params.client_id = client_id;
    params.exe_path = exe_path;
    params.media_enabled = media_enabled;
    params.io_dispatch = &m_task_queue;
    params.post_to_signaling_thread = [this](std::function<void()> fn) { m_task_queue.dispatch(std::move(fn)); };
    params.send_signaling_json = [this](const std::string& json) { m_transport.send_json_text(json); };
    params.on_connection_lost = [this]() { on_operator_connection_lost(); };

    m_active_session = m_factory.create_session(params);
    std::cout << "[session_director] new session client_id=" << client_id << " media=" << media_enabled << std::endl;
}

void session_director::cancel_pending_disconnect_teardown()
{
    m_disconnect_teardown_pending = false;
    ++m_disconnect_generation;
    if (m_active_session) m_active_session->end_disconnect_grace();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          原子移出当前会话 shared_ptr 并调用 teardown，清空当前 client_id
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void session_director::teardown_active()
{
    auto session = std::move(m_active_session);
    m_current_client_id.clear();
    if (session) session->teardown();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          操作者 PeerConnection 断开时排队执行 teardown_active，释放进程与媒体资源
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void session_director::on_operator_connection_lost()
{
    const uint64_t gen = ++m_disconnect_generation;
    m_task_queue.dispatch([this, gen]() {
        if (m_current_client_id.empty() || !m_active_session) return;
        m_disconnect_teardown_pending = true;
        m_active_session->begin_disconnect_grace();
        std::cout << "[session_director] operator connection lost client_id=" << m_current_client_id
                  << " pending_teardown_s=60" << std::endl;
        std::thread([this, gen]() {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            m_task_queue.dispatch([this, gen]() {
                if (gen != m_disconnect_generation.load()) return;
                if (!m_disconnect_teardown_pending) return;
                std::cout << "[session_director] disconnect grace expired, tearing down client_id="
                          << m_current_client_id << std::endl;
                teardown_active();
            });
        }).detach();
    });
}
