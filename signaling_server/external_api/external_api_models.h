////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 对外接口请求 / 响应 / 转发载荷数据结构（值对象，与《接口.docx》字段对齐）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 使用 Plain struct 表达 JSON 嵌套，字段命名与协议文档一致（snake_case）
// - 不含行为逻辑，便于序列化与测试构造
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef EXTERNAL_API_MODELS_H
#define EXTERNAL_API_MODELS_H

#include <QString>
#include <QStringList>
#include <QVector>

namespace external_api_models {

struct login_data {
	QString username;
	QString password;
};

struct login_res_body {
	QString token;
	QString role;
};

struct node_register_data {
	QString node_name;
	QString auth_key;
};

struct auth_app_item {
	qint64 app_id = 0;
	QString display_name;
	QString node_name;
	/** 可执行路径（供前端发起 legacy request / 编排会话） */
	QString exe_path;
};

struct auth_apps_res_body {
	QVector<auth_app_item> apps;
};

struct app_icon_item {
	qint64 app_id = 0;
	QString icon_base64;
};

struct apps_icons_res_body {
	QVector<app_icon_item> icons;
};

struct admin_add_app_data {
	QString node_name;
	QString display_name;
	QString exe_path;
	QString icon_base64;
	int is_public = 0;
};

struct admin_add_app_res_body {
	qint64 app_id = 0;
	QString role;
};

struct session_request_data {
	qint64 app_id = 0;
	bool is_reconnect = false;
};

struct agent_start_session_data {
	QString exe_path;
	QString session_id;
	int grace_period = 0;
	/** 操作端（浏览器）在信令服务上登记的 WebSocket id，供 Agent 将 offer/信令发回该连接 */
	QString operator_client_id;
};

struct webrtc_offer_data {
	QString sdp;
	QString session_id;
};

struct session_status_change_data {
	QString status;
	QString message;
	qint64 timestamp = 0;
};

struct usage_rank_item {
	qint64 app_id = 0;
	qint64 total_time = 0;
};

struct admin_stats_res_body {
	int active_sessions = 0;
	QStringList nodes_online;
	QVector<usage_rank_item> usage_ranking;
};

/** 管控台 get_admin_directory 单条：采集节点 */
struct admin_directory_node_item {
	qint64 node_id = 0;
	QString node_name;
	int is_online = 0;
	QString last_seen;
};

/** 用户（不含 password_hash） */
struct admin_directory_user_item {
	qint64 user_id = 0;
	QString username;
	QString role;
};

/** 全库应用（按节点展开） */
struct admin_directory_app_item {
	qint64 app_id = 0;
	qint64 node_id = 0;
	QString node_name;
	QString display_name;
	QString exe_path;
	int is_public = 0;
};

/** 用户-应用 授权关系（人员与应用程序配置） */
struct admin_directory_perm_item {
	qint64 perm_id = 0;
	qint64 user_id = 0;
	QString username;
	qint64 app_id = 0;
	QString app_display_name;
	QString node_name;
};

struct admin_directory_res_body {
	QVector<admin_directory_node_item> nodes;
	QVector<admin_directory_user_item> users;
	QVector<admin_directory_app_item> apps;
	QVector<admin_directory_perm_item> permissions;
};

struct file_entry {
	QString name;
	bool is_dir = false;
	qint64 size = 0;
	qint64 mtime = 0;
};

struct file_list_data {
	QString path;
};

struct file_list_res_body {
	QString current_path;
	QVector<file_entry> entries;
};

struct file_download_init_data {
	QString path;
	int chunk_size = 16384;
};

struct file_download_data_body {
	QString transfer_id;
	qint64 offset = 0;
	QString data_base64;
	bool is_eof = false;
};

struct file_upload_chunk_data {
	QString transfer_id;
	qint64 offset = 0;
	QString data_base64;
};

struct file_upload_ack_data {
	QString transfer_id;
	qint64 next_expected_offset = 0;
	bool is_complete = false;
};

struct control_action_data {
	QString action;
	bool force = false;
};

struct latency_ping_data {
	qint64 client_ts = 0;
	int seq = 0;
};

struct latency_pong_data {
	qint64 client_ts = 0;
	qint64 server_ts = 0;
	int seq = 0;
};

} // namespace external_api_models

#endif
