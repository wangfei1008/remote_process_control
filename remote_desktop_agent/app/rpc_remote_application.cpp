#include "app/rpc_remote_application.h"

#include "rtc/rtc.hpp"

#include <iostream>

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          构造应用对象：加载配置、初始化 RTC 日志，创建信令传输与会话编排并建立回调
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
rpc_remote_application::rpc_remote_application()
    : m_settings(runtime_settings::load_from_environment())
{
    rtc::InitLogger(rtc::LogLevel::Info);
    m_transport = std::make_unique<signaling_transport>(m_settings);
    m_director = std::make_unique<session_director>(m_settings, *m_transport);
    m_transport->set_observer(m_director.get());
    m_transport->set_on_transport_closed([this]() {
        if (m_director) m_director->stop_all_sessions();
    });
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          析构前解除信令对象上的观察者与关闭回调，避免悬空指针
/// @参数
///          无
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
rpc_remote_application::~rpc_remote_application()
{
    if (m_transport) {
        m_transport->set_on_transport_closed(nullptr);
        m_transport->set_observer(nullptr);
    }
}

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          连接信令服务器并阻塞运行；用户输入一行后停止所有会话
/// @参数
///          signaling_ip--信令服务器 IP 或主机名
///          signaling_port--信令 WebSocket 端口
/// @返回值
///          无
///
/// @时间    2026/4/3
/////////////////////////////////////////////////////////////////////////////
void rpc_remote_application::run(const std::string& signaling_ip, int signaling_port)
{
    m_transport->start(signaling_ip, signaling_port);
    std::cout << "[app] signaling ready, enter to exit" << std::endl;
    std::string line;
    std::cin >> line;
    m_director->stop_all_sessions();
}
