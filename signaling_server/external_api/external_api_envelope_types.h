////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 对外接口「信封」类型聚合（std::variant），配合工厂解析 JSON
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 每种 arm 对应文档中的一种 type + 载荷形态（请求 / 响应 / 转发）
// - 序列化侧使用 std::visit 分发，避免巨型 switch 与 void* 
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef EXTERNAL_API_ENVELOPE_TYPES_H
#define EXTERNAL_API_ENVELOPE_TYPES_H

#include "external_api_models.h"

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <variant>

namespace external_api_envelope_types {

using namespace external_api_models;

struct envelope_login {
	login_data data;
};

struct envelope_login_res {
	bool success = false;
	login_res_body data;
};

struct envelope_node_register {
	node_register_data data;
};

struct envelope_get_auth_apps {
	QString token;
};

struct envelope_auth_apps_res {
	auth_apps_res_body body;
};

struct envelope_get_apps_icons {
	QString token;
	QVector<qint64> app_ids;
};

struct envelope_apps_icons_res {
	QVector<app_icon_item> icons;
};

struct envelope_admin_add_app_req {
	QString token;
	admin_add_app_data data;
};

struct envelope_admin_add_app_res {
	bool success = false;
	admin_add_app_res_body data;
};

struct envelope_session_request {
	QString token;
	session_request_data data;
};

struct envelope_agent_start_session {
	agent_start_session_data data;
};

struct envelope_webrtc_offer {
	webrtc_offer_data data;
};

struct envelope_session_status_change {
	session_status_change_data data;
};

struct envelope_get_admin_stats {
	QString token;
};

struct envelope_admin_stats_res {
	admin_stats_res_body data;
};

struct envelope_get_admin_directory {
	QString token;
};

struct envelope_admin_directory_res {
	admin_directory_res_body data;
};

/** 管控台统一变更：节点 / 应用 / 用户 / 权限（增删改） */
struct admin_mutate_req_data {
	QString entity;
	QString action;
	QJsonObject payload;
};

struct envelope_admin_mutate_req {
	QString token;
	admin_mutate_req_data data;
};

struct envelope_admin_mutate_res {
	bool success = false;
	QString message;
	QString entity;
	qint64 id = 0;
};

struct envelope_file_list_req {
	QString token;
	file_list_data data;
};

struct envelope_file_list_res {
	file_list_res_body data;
};

struct envelope_file_download_init {
	file_download_init_data data;
};

struct envelope_file_download_data {
	file_download_data_body data;
};

struct envelope_file_upload_chunk {
	file_upload_chunk_data data;
};

struct envelope_file_upload_ack {
	bool success = false;
	file_upload_ack_data data;
};

struct envelope_control_action {
	control_action_data data;
};

struct envelope_latency_ping {
	latency_ping_data data;
};

struct envelope_latency_pong {
	latency_pong_data data;
};

using external_api_message_variant = std::variant<
    envelope_login,
    envelope_login_res,
    envelope_node_register,
    envelope_get_auth_apps,
    envelope_auth_apps_res,
    envelope_get_apps_icons,
    envelope_apps_icons_res,
    envelope_admin_add_app_req,
    envelope_admin_add_app_res,
    envelope_session_request,
    envelope_agent_start_session,
    envelope_webrtc_offer,
    envelope_session_status_change,
    envelope_get_admin_stats,
    envelope_admin_stats_res,
    envelope_get_admin_directory,
    envelope_admin_directory_res,
    envelope_admin_mutate_req,
    envelope_admin_mutate_res,
    envelope_file_list_req,
    envelope_file_list_res,
    envelope_file_download_init,
    envelope_file_download_data,
    envelope_file_upload_chunk,
    envelope_file_upload_ack,
    envelope_control_action,
    envelope_latency_ping,
    envelope_latency_pong>;

} // namespace external_api_envelope_types

#endif
