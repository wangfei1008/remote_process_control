////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 信令 WebSocket 文本消息业务处理（登录鉴权、节点注册、会话编排、统计与 JSON 中继）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 解析《接口》定义的 JSON 信封（external_api_protocol_facade），调用 manage_db 完成持久化查询
// - 对非业务类报文（如 webrtc_offer）支持按根字段 id 转发（SignalingRelay）
// - 节点断开时尝试将 nodes 表标记离线
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef SIGNALINGAPIHANDLER_H
#define SIGNALINGAPIHANDLER_H

#include "db/manage_db.h"
#include "external_api/external_api_envelope_types.h"
#include "external_api/external_api_protocol_facade.h"

#include <QHash>
#include <QString>

#include <optional>

class QWebSocket;
class ClientRegistry;
class SignalingRelay;

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          单连接上的登录态（内存令牌映射）
/// @参数
///          无
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
struct signaling_token_record {
	qint64 user_id = 0;
	QString username;
	QString role;
};

/////////////////////////////////////////////////////////////////////////////
/// @说明
///          信令 JSON 业务处理器（非 QObject，避免 moc）
/// @参数
///          无
/// @返回值 无
///
/// @时间    2026/5/7
/////////////////////////////////////////////////////////////////////////////
class signaling_api_handler {
public:
	explicit signaling_api_handler(ClientRegistry *client_registry, SignalingRelay *signaling_relay);

	/////////////////////////////////////////////////////////////////////////////
	/// @说明
	///          打开 SQLite 数据库（manage.db）
	/// @参数
	///          db_path--数据库文件路径（Qt 路径）
	/// @返回值 正常值。0:成功，小于0对应错误码（manage_db_err）
	///
	/// @时间    2026/5/7
	/////////////////////////////////////////////////////////////////////////////
	int open_database(const QString &db_path);

	/////////////////////////////////////////////////////////////////////////////
	/// @说明
	///          处理 WebSocket 文本帧（JSON）
	/// @参数
	///          socket--连接；message--UTF-16 JSON 文本
	/// @返回值 无
	///
	/// @时间    2026/5/7
	/////////////////////////////////////////////////////////////////////////////
	void handle_text_message(QWebSocket *socket, const QString &message);

	/////////////////////////////////////////////////////////////////////////////
	/// @说明
	///          连接断开回调（URL path 登记的 client_id）
	/// @参数
	///          client_id--连接标识，与 ClientRegistry 键一致
	/// @返回值 无
	///
	/// @时间    2026/5/7
	/////////////////////////////////////////////////////////////////////////////
	void on_socket_disconnected(const QString &client_id);

private:
	std::optional<signaling_token_record> resolve_token(const QString &token) const;
	bool user_can_access_application(qint64 user_id, const application_row &application);
	void send_json(QWebSocket *socket, const external_api_envelope_types::external_api_message_variant &message);

	void handle_envelope_login(QWebSocket *socket, const external_api_envelope_types::envelope_login &envelope);
	void handle_envelope_node_register(QWebSocket *socket,
	                                   const external_api_envelope_types::envelope_node_register &envelope);
	void handle_envelope_get_auth_apps(QWebSocket *socket,
	                                     const external_api_envelope_types::envelope_get_auth_apps &envelope);
	void handle_envelope_get_apps_icons(QWebSocket *socket,
	                                    const external_api_envelope_types::envelope_get_apps_icons &envelope);
	void handle_envelope_admin_add_app_req(QWebSocket *socket,
	                                         const external_api_envelope_types::envelope_admin_add_app_req &envelope);
	void handle_envelope_session_request(QWebSocket *socket,
	                                     const external_api_envelope_types::envelope_session_request &envelope);
	void handle_envelope_get_admin_stats(QWebSocket *socket,
	                                     const external_api_envelope_types::envelope_get_admin_stats &envelope);
	void handle_envelope_get_admin_directory(
	    QWebSocket *socket, const external_api_envelope_types::envelope_get_admin_directory &envelope);
	void handle_envelope_admin_mutate_req(QWebSocket *socket,
	                                      const external_api_envelope_types::envelope_admin_mutate_req &envelope);
	void handle_envelope_latency_ping(QWebSocket *socket,
	                                  const external_api_envelope_types::envelope_latency_ping &envelope);

	ClientRegistry *m_client_registry = nullptr;
	SignalingRelay *m_signaling_relay = nullptr;
	manage_db m_manage_db;
	external_api_protocol_facade m_protocol_facade;
	QHash<QString, signaling_token_record> m_token_table;
};

#endif
