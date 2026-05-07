////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 对外信令 / DataChannel 协议字符串常量（与《接口.docx》一致）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 集中定义 JSON 报文中 type 字段取值，避免魔法字符串散落
// - 仅描述协议层，不参与业务转发与持久化
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef EXTERNAL_API_PROTOCOL_CONSTANTS_H
#define EXTERNAL_API_PROTOCOL_CONSTANTS_H

#include <QString>

namespace external_api_protocol_constants {

inline const QString k_type_login() { return QStringLiteral("login"); }
inline const QString k_type_login_res() { return QStringLiteral("login_res"); }
inline const QString k_type_node_register() { return QStringLiteral("node_register"); }
inline const QString k_type_get_auth_apps() { return QStringLiteral("get_auth_apps"); }
inline const QString k_type_auth_apps_res() { return QStringLiteral("auth_apps_res"); }
inline const QString k_type_get_apps_icons() { return QStringLiteral("get_apps_icons"); }
inline const QString k_type_apps_icons_res() { return QStringLiteral("apps_icons_res"); }
inline const QString k_type_admin_add_app() { return QStringLiteral("admin_add_app"); }
inline const QString k_type_session_request() { return QStringLiteral("session_request"); }
inline const QString k_type_agent_start_session() { return QStringLiteral("agent_start_session"); }
inline const QString k_type_webrtc_offer() { return QStringLiteral("webrtc_offer"); }
inline const QString k_type_session_status_change() { return QStringLiteral("session_status_change"); }
inline const QString k_type_get_admin_stats() { return QStringLiteral("get_admin_stats"); }
inline const QString k_type_admin_stats_res() { return QStringLiteral("admin_stats_res"); }
inline const QString k_type_get_admin_directory() { return QStringLiteral("get_admin_directory"); }
inline const QString k_type_admin_directory_res() { return QStringLiteral("admin_directory_res"); }
inline const QString k_type_admin_mutate() { return QStringLiteral("admin_mutate"); }
inline const QString k_type_admin_mutate_res() { return QStringLiteral("admin_mutate_res"); }
inline const QString k_type_file_list_req() { return QStringLiteral("file_list_req"); }
inline const QString k_type_file_list_res() { return QStringLiteral("file_list_res"); }
inline const QString k_type_file_download_init() { return QStringLiteral("file_download_init"); }
inline const QString k_type_file_download_data() { return QStringLiteral("file_download_data"); }
inline const QString k_type_file_upload_chunk() { return QStringLiteral("file_upload_chunk"); }
inline const QString k_type_file_upload_ack() { return QStringLiteral("file_upload_ack"); }
inline const QString k_type_control_action() { return QStringLiteral("control_action"); }
inline const QString k_type_latency_ping() { return QStringLiteral("latency_ping"); }
inline const QString k_type_latency_pong() { return QStringLiteral("latency_pong"); }

} // namespace external_api_protocol_constants

#endif
