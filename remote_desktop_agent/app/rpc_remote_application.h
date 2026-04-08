////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 远程桌面 Agent 应用入口（组装信令与会话编排）
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- 加载 runtime_settings，初始化 RTC 日志
//- 创建 signaling_transport 与 session_director 并建立 Observer 关系
//- run：连接信令后阻塞直至用户退出，并停止所有会话
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "bootstrap/runtime_settings.h"
#include "signaling/signaling_transport.h"
#include "orchestration/session_director.h"

#include <memory>

class rpc_remote_application {
public:
    rpc_remote_application();
    ~rpc_remote_application();
    void run(const std::string& signaling_ip, int signaling_port);

private:
    runtime_settings m_settings;
    std::unique_ptr<signaling_transport> m_transport;
    std::unique_ptr<session_director> m_director;
};
