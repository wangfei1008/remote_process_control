#include "orchestration/active_desktop_session.h"
#include "media/media_pipeline.h"
#include "webrtc/operator_channel.h"

#include "nlohmann/json.hpp"

#include <chrono>
#include <iostream>
#include <utility>

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          析构：若尚未 teardown 则补做一次，确保资源释放
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
active_desktop_session::~active_desktop_session()
{
    if (!m_torn_down) teardown();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          装配本会话：文件服务、可选媒体管线、operator_channel、挂接采集回调并发起本地 SDP
/// @参数
///          params--工厂传入的创建参数
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void active_desktop_session::wire_components(const desktop_session_create_params& params)
{
    m_settings = params.settings;
    m_client_id = params.client_id;
    m_exe_path = params.exe_path;
    m_media_enabled = params.media_enabled;
    m_post_to_signaling = params.post_to_signaling_thread;
    m_on_connection_lost = params.on_connection_lost;

    remote_file_transfer_controller::options opts;
    opts.data_root = m_settings->data_root;
    opts.default_chunk_size = static_cast<std::size_t>(m_settings->file_chunk_size);
    m_file_service = std::make_shared<remote_file_transfer_controller>(std::move(opts));

    if (m_media_enabled) {
        m_media = std::make_unique<media_pipeline>(m_exe_path, *m_settings);
    }

    create_or_replace_operator_channel(params);
}

void active_desktop_session::reconnect_operator(const desktop_session_create_params& params)
{
    if (m_torn_down) return;
    if (params.client_id != m_client_id) return;
    m_post_to_signaling = params.post_to_signaling_thread;
    m_on_connection_lost = params.on_connection_lost;
    end_disconnect_grace();
    create_or_replace_operator_channel(params);
}

void active_desktop_session::begin_disconnect_grace()
{
    m_allow_stop_media_if_no_clients = false;
}

void active_desktop_session::end_disconnect_grace()
{
    m_allow_stop_media_if_no_clients = true;
}

void active_desktop_session::create_or_replace_operator_channel(const desktop_session_create_params& params)
{
    if (m_operator) m_operator->close();
    m_operator.reset();

    auto self = shared_from_this();
    auto create_media_session = [self]() -> std::shared_ptr<remote_desktop_media_session> {
        return self->create_media_session_for_peer();
    };

    m_operator = std::make_shared<operator_channel>(params.rtc_config, params.websocket, m_client_id, m_file_service,
        m_media_enabled, std::move(create_media_session), params.send_signaling_json, *params.io_dispatch,
        params.on_connection_lost);

    m_operator->start_local_negotiation();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          供 ClientPeerConnection 回调：创建或复用共享 remote_desktop_media_session，并挂接退出与停流
/// @参数
///          无
/// @返回值
///          媒体会话；无媒体管线或 exe 为空时可能为 nullptr
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
std::shared_ptr<remote_desktop_media_session> active_desktop_session::create_media_session_for_peer()
{
    if (!m_media) return nullptr;
    auto self = shared_from_this();
    return m_media->get_or_create_media_session(
        [self]() { self->handle_remote_process_exit(); },
        [self](const char* why, uint64_t missing_ms) { self->on_remote_window_missing(why, missing_ms); },
        [self]() {
            if (!self->m_post_to_signaling) return;
            self->m_post_to_signaling([self]() { self->on_stop_if_no_clients_sample(); });
        });
}

void active_desktop_session::on_remote_window_missing(const char* why, uint64_t missing_ms)
{
    if (!m_post_to_signaling) return;
    const std::string why_s = (why ? why : "");
    m_post_to_signaling([self = shared_from_this(), why_s, missing_ms]() {
        self->broadcast_remote_window_missing(why_s.c_str(), missing_ms);
    });
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          被控进程退出：在信令线程上广播 remoteProcessExited、解绑鼠标目标并停止媒体流
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void active_desktop_session::handle_remote_process_exit()
{
    if (!m_post_to_signaling) return;
    m_post_to_signaling([self = shared_from_this()]() {
        std::cout << "[session] handle_remote_process_exit client_id=" << self->m_client_id
                  << " exePath=" << self->m_exe_path
                  << " media=" << (self->m_media_enabled ? 1 : 0)
                  << " has_operator=" << (self->m_operator ? 1 : 0)
                  << " has_peer=" << ((self->m_operator && self->m_operator->client_peer()) ? 1 : 0)
                  << std::endl;
        self->broadcast_remote_process_exited();
        if (self->m_operator && self->m_operator->client_peer())
            self->m_operator->client_peer()->release_mouse_target_binding();
        if (self->m_media) self->m_media->stop_media_session();
    });
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          媒体发送路径回调：当操作者 peer 不可用则停止采集管线，避免空转
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void active_desktop_session::on_stop_if_no_clients_sample()
{
    if (!m_media) return;
    if (!m_allow_stop_media_if_no_clients) return;

    const auto now = std::chrono::steady_clock::now();
    if (m_operator && m_operator->client_peer()) {
        m_last_peer_seen = now;
        m_stop_called = false;
        return;
    }

    if (m_stop_called) return;

    const auto elapsed_ms =
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_peer_seen).count());

    // Log at most once every 5 seconds to avoid spam.
    if (now - m_last_stop_check_log >= std::chrono::seconds(5)) {
        m_last_stop_check_log = now;
        std::cout << "[session] no operator peer for " << elapsed_ms << "ms (grace=" << m_no_client_grace_ms
                  << "ms) client_id=" << m_client_id << std::endl;
    }

    if (elapsed_ms < m_no_client_grace_ms) return;

    m_stop_called = true;
    std::cout << "[session] no-client grace expired, stopping media client_id=" << m_client_id << std::endl;
    m_media->stop_media_session();
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          通过 DataChannel 向前端发送 type=remoteProcessExited 的 JSON 通知
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void active_desktop_session::broadcast_remote_process_exited()
{
    const nlohmann::json msg = { { "type", "remoteProcessExited" } };
    const std::string payload = msg.dump();
    const bool has_peer = (m_operator && m_operator->client_peer());
    std::cout << "[session] send remoteProcessExited client_id=" << m_client_id
              << " has_peer=" << (has_peer ? 1 : 0) << std::endl;
    if (!has_peer) return;
    auto ch = m_operator->client_peer()->get_data_channel();
    if (!ch) {
        std::cout << "[session] remoteProcessExited skipped (no dc) client_id=" << m_client_id << std::endl;
        return;
    }
    try {
        ch->send(payload);
    } catch (...) {
        std::cout << "[session] remoteProcessExited send failed client_id=" << m_client_id << std::endl;
    }
}

void active_desktop_session::broadcast_remote_window_missing(const char* why, uint64_t missing_ms)
{
    const bool has_peer = (m_operator && m_operator->client_peer());
    if (!has_peer) return;
    auto ch = m_operator->client_peer()->get_data_channel();
    if (!ch) return;

    const nlohmann::json msg = {
        { "type", "remoteWindowMissing" },
        { "why", (why ? std::string(why) : std::string()) },
        { "missingMs", missing_ms },
    };
    const std::string payload = msg.dump();

    // Throttle log to avoid spamming (message itself is already throttled upstream).
    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_window_missing_log >= std::chrono::seconds(2)) {
        m_last_window_missing_log = now;
        std::cout << "[session] send remoteWindowMissing client_id=" << m_client_id
                  << " missing_ms=" << missing_ms
                  << " why=" << (why ? why : "") << std::endl;
    }
    try {
        ch->send(payload);
    } catch (...) {
    }
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          将信令侧 SDP Answer 设置到当前 PeerConnection
/// @参数
///          sdp_text--Answer SDP 文本
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void active_desktop_session::apply_remote_answer(const std::string& sdp_text)
{
    if (m_operator) m_operator->set_remote_answer(sdp_text);
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          幂等拆除：广播退出、停流、解绑输入、关闭 PeerConnection 并释放子系统句柄
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void active_desktop_session::teardown()
{
    if (m_torn_down) return;
    m_torn_down = true;
    end_disconnect_grace();
    std::cout << "[session] teardown client_id=" << m_client_id << " exePath=" << m_exe_path
              << " media=" << (m_media_enabled ? 1 : 0) << std::endl;
    broadcast_remote_process_exited();
    if (m_media) m_media->stop_media_session();
    if (m_operator && m_operator->client_peer()) m_operator->client_peer()->release_mouse_target_binding();
    if (m_operator) m_operator->close();
    m_operator.reset();
    m_media.reset();
    m_file_service.reset();
}
