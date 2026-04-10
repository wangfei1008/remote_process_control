////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 信令事件编排与会话生命周期（单一媒体会话 + 新请求替换）
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- 实现 signaling_observer：根据 signaling_event 创建/切换/关闭 active_desktop_session
//- 维护 rtc::Configuration（STUN 等），将 send / post 回调注入新会话
//- replace_with_new_session：先 teardown 再建新的，满足「新请求必须替换」
//- sdp_answer 仅投递给当前 m_current_client_id 对应会话；stop 与连接丢失时统一清理
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "signaling/signaling_observer.h"
#include "signaling/signaling_transport.h"
#include "orchestration/desktop_session_factory.h"
#include "orchestration/session_replace_policy.h"
#include "bootstrap/runtime_settings.h"
#include "transport/dispatch_queue.hpp"

#include "rtc/rtc.hpp"

#include <memory>
#include <string>
#include <atomic>

class session_director : public signaling_observer {
public:
    session_director(runtime_settings settings, signaling_transport& transport);

    void on_signaling_event(const signaling_event& event) override;
    void stop_all_sessions();

private:
    void replace_with_new_session(const std::string& client_id, const std::string& exe_path, bool media_enabled);
    void teardown_active();
    void on_operator_connection_lost();
    void cancel_pending_disconnect_teardown();

    runtime_settings m_settings;
    signaling_transport& m_transport;
    DispatchQueue m_task_queue;
    rtc::Configuration m_rtc_config;
    std::unique_ptr<session_replace_policy> m_replace_policy;
    desktop_session_factory m_factory;
    std::shared_ptr<active_desktop_session> m_active_session;
    std::string m_current_client_id;

    std::atomic<uint64_t> m_disconnect_generation{0};
    bool m_disconnect_teardown_pending = false;
};
