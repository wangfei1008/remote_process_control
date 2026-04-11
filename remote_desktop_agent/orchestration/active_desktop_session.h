////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 单次远程桌面会话门面（Facade 模式）
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- 聚合本会话内：文件服务、可选 media_pipeline、operator_channel（单 PeerConnection）
//- wire_components：配置 FileTransferService、挂接采集流工厂、启动本地 SDP 协商
//- 处理远端进程退出：广播 remoteProcessExited、解绑输入、停止媒体管线（不断信令）
//- teardown：幂等拆除；由 session_director 保证全局至多存活一个实例，新请求先替换
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "orchestration/desktop_session_factory.h"
#include "media/media_pipeline.h"
#include "transport/remote_file_transfer_controller.h"

#include <chrono>
#include <memory>
#include <string>

class operator_channel;
class remote_desktop_media_session;

class active_desktop_session : public std::enable_shared_from_this<active_desktop_session> {
public:
    ~active_desktop_session();

    void wire_components(const desktop_session_create_params& params);
    // Recreate operator_channel for the same session (reconnect) without restarting remote process.
    void reconnect_operator(const desktop_session_create_params& params);
    const std::string& exe_path() const noexcept { return m_exe_path; }
    // When operator connection is lost, keep media/process alive for a grace period.
    void begin_disconnect_grace();
    void end_disconnect_grace();
    void teardown();

    void apply_remote_answer(const std::string& sdp_text);

private:
    friend class desktop_session_factory;
    active_desktop_session() = default;

    void create_or_replace_operator_channel(const desktop_session_create_params& params);
    std::shared_ptr<remote_desktop_media_session> create_media_session_for_peer();
    void handle_remote_process_exit();
    void on_remote_window_missing(const char* why, uint64_t missing_ms);
    void on_stop_if_no_clients_sample();
    void broadcast_remote_process_exited();
    void broadcast_remote_window_missing(const char* why, uint64_t missing_ms);

    const runtime_settings* m_settings = nullptr;
    std::string m_client_id;
    std::string m_exe_path;
    bool m_media_enabled = true;
    std::function<void(std::function<void()>)> m_post_to_signaling;
    std::function<void()> m_on_connection_lost;

    std::shared_ptr<remote_file_transfer_controller> m_file_service;
    std::unique_ptr<media_pipeline> m_media;
    std::shared_ptr<operator_channel> m_operator;
    bool m_torn_down = false;
    bool m_allow_stop_media_if_no_clients = true;

    std::chrono::steady_clock::time_point m_last_peer_seen = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point m_last_stop_check_log = std::chrono::steady_clock::now();
    bool m_stop_called = false;
    uint32_t m_no_client_grace_ms = 60000;

    std::chrono::steady_clock::time_point m_last_window_missing_log = std::chrono::steady_clock::now();
};
