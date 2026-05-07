////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 信令 JSON 解析后的领域事件结构
//
// 作者：WangFei
// 时间： 2026-04-03
// 修改:
//              1、2026-04-03创建
//
//详细功能说明：
//- 将 WebSocket 消息中的 type/id/exePath/sdp 等字段整理为 C++ 侧可分发的事件
//- signaling_transport 只负责解析与填充，不解释业务语义；业务由 session_director 处理
//- 支持：媒体会话请求、仅文件会话、SDP Answer、停止会话等事件类型
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>

enum class signaling_event_type {
    invalid,
    media_session_requested,
    file_only_session_requested,
    sdp_answer,
    stop_session,
    /** 信令服务经库鉴权后下发的受管会话开始（见接口：agent_start_session） */
    agent_start_session,
};

struct signaling_event {
    signaling_event_type type = signaling_event_type::invalid;
    std::string client_id;
    std::string exe_path;
    std::string sdp_text;
    /** 服务器生成的会话 id，用于审计与排查 */
    std::string signaling_session_id;
    /** agent_start_session.data.grace_period；其它事件为 0 表示使用 runtime 默认 */
    int grace_period_sec = 0;
};
