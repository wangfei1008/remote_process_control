#include "media/media_pipeline.h"

#include <algorithm>
#include <iostream>

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          构造媒体管线：保存待启动进程路径与媒体会话配置
/// @参数
///          exe_path--远程协议侧要求的被控程序路径
///          stream_settings--含 frame_mark_interval、capture_health_interval 等
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
media_pipeline::media_pipeline(std::string exe_path, runtime_settings stream_settings)
    : m_exe_path(std::move(exe_path))
    , m_stream_settings(std::move(stream_settings))
{
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          懒创建 remote_desktop_media_session：拉起远程进程、准备音视频与发送链路
/// @参数
///          on_remote_process_exit--进程退出时回调（由采集侧线程触发，一般需 post）
///          stop_if_no_clients--发送路径在需要时调用的停流检查
/// @返回值
///          已创建的媒体会话；exe 为空等情况下返回 nullptr
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
std::shared_ptr<remote_desktop_media_session> media_pipeline::get_or_create_media_session(
    std::function<void()> on_remote_process_exit,
    std::function<void(const char* why, uint64_t missing_ms)> on_window_missing,
    std::function<void()> stop_if_no_clients)
{
    {
        std::scoped_lock lk(m_mutex);
        if (m_media_session) return m_media_session;
    }

    {
        std::scoped_lock lk(m_mutex);
        if (m_exe_path.empty()) return nullptr;
    }
    std::cout << "[media_pipeline] create media session exePath=" << m_exe_path << std::endl;

    auto session = std::make_shared<remote_desktop_media_session>(m_exe_path, m_stream_settings,
        std::move(on_remote_process_exit), std::move(on_window_missing), std::move(stop_if_no_clients));

    {
        std::scoped_lock lk(m_mutex);
        if (!m_media_session) m_media_session = session;
    }
    return m_media_session;
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          停止并释放当前媒体会话
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void media_pipeline::stop_media_session()
{
    std::shared_ptr<remote_desktop_media_session> local;
    {
        std::scoped_lock lk(m_mutex);
        local = m_media_session;
        m_media_session.reset();
    }
    if (local) {
        try {
            local->stop_media_session();
        } catch (...) {
        }
    }
}
