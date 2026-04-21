#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <windows.h>
#include <vector>

#include "bootstrap/runtime_settings.h"

class ClientPeerConnection;
struct remote_desktop_media_session_impl;

// 媒体会话：单会话音视频与发送调度
// - 对外：attach_client_peer、start/stop、request_force_keyframe、get_main_window
// - 对内：remote_video_engine（采集+编码+遥测）、silence_opus_generator、remote_desktop_media_sender
class remote_desktop_media_session : public std::enable_shared_from_this<remote_desktop_media_session> {
public:
    remote_desktop_media_session(const std::string& exe_path,
                                   runtime_settings stream_settings,
                                   std::function<void()> on_remote_process_exit,
                                   std::function<void(const char* why, uint64_t missing_ms)> on_window_missing,
                                   std::function<void()> stop_if_no_clients);
    ~remote_desktop_media_session();

    void attach_client_peer(const std::weak_ptr<ClientPeerConnection>& client_peer);

    void start_media_session();
    void stop_media_session();

    void request_force_keyframe();

    bool is_media_running() const;
    HWND get_main_window() const;

private:
    mutable std::mutex m_mutex;

    runtime_settings m_stream_settings;

    std::weak_ptr<ClientPeerConnection> m_client_peer;

    std::function<void()> m_on_remote_process_exit;
    std::function<void(const char* why, uint64_t missing_ms)> m_on_window_missing;
    std::function<void()> m_stop_if_no_clients;

    std::unique_ptr<remote_desktop_media_session_impl> m_impl;
};

