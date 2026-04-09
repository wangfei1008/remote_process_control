////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 桌面会话工厂与会话构造参数（Factory 模式）
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- desktop_session_create_params：打包创建一次 active_desktop_session 所需的信令、RTC 配置、回调
//- desktop_session_factory::create_session：两阶段构造（先 shared_ptr 再 wire_components），以支持 enable_shared_from_this
//- active_desktop_session 前向声明，完整类型在 .cpp 与 active_desktop_session.h 中衔接
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "rtc/rtc.hpp"
#include "transport/dispatch_queue.hpp"
#include "bootstrap/runtime_settings.h"

#include <functional>
#include <memory>
#include <string>

class active_desktop_session;

struct desktop_session_create_params {
    const runtime_settings* settings = nullptr;
    rtc::Configuration rtc_config;
    std::weak_ptr<rtc::WebSocket> websocket;
    std::string client_id;
    std::string exe_path;
    bool media_enabled = true;
    DispatchQueue* io_dispatch = nullptr;
    std::function<void(std::function<void()>)> post_to_signaling_thread;
    std::function<void(const std::string&)> send_signaling_json;
    std::function<void()> on_connection_lost;
};

class desktop_session_factory {
public:
    std::shared_ptr<active_desktop_session> create_session(const desktop_session_create_params& params) const;
};
