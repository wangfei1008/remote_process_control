////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明：单会话共享音视频管线（remote_desktop_media_session + 进程路径与运行时配置）
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- 懒创建 remote_desktop_media_session：内含 remote_video_engine、静音 Opus、remote_desktop_media_sender
//- 通过 operator_channel / ClientPeerConnection 挂接 peer，满足「单媒体会话、新连接可替换」模型
//- 远端进程退出与「无客户端」时由会话与 sender 回调停流，行为对齐旧版 RpcMediaSession
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "bootstrap/runtime_settings.h"
#include "session/remote_desktop_media_session.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

class media_pipeline {
public:
    explicit media_pipeline(std::string exe_path, runtime_settings stream_settings);

    std::shared_ptr<remote_desktop_media_session> get_or_create_media_session(
        std::function<void()> on_remote_process_exit,
        std::function<void()> stop_if_no_clients);

    void stop_media_session();

    std::string exe_path() const
    {
        std::scoped_lock lk(m_mutex);
        return m_exe_path;
    }

private:
    mutable std::mutex m_mutex;
    std::string m_exe_path;
    runtime_settings m_stream_settings;
    std::shared_ptr<remote_desktop_media_session> m_media_session;
};
