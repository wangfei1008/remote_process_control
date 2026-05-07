////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： Qt WebSocket 信令服务门面（监听、连接登记、文本消息派发）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 自 URL path 解析 client_id 并登记到 ClientRegistry
// - 文本 JSON 交由 signaling_api_handler（数据库 + 协议 + 条件中继）
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef SIGNALINGSERVER_H
#define SIGNALINGSERVER_H

#include <QAbstractSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include "core/ClientRegistry.h"
#include "core/SignalingApiHandler.h"
#include "core/SignalingRelay.h"

class QWebSocketServer;
class QWebSocket;

class SignalingServer : public QObject {
	Q_OBJECT
public:
	explicit SignalingServer(QObject *parent = nullptr);

	/////////////////////////////////////////////////////////////////////////////
	/// @说明
	///          打开管理库 SQLite（manage.db）
	/// @参数
	///          db_path--数据库文件路径
	/// @返回值 正常值。0:成功，小于0对应错误码
	///
	/// @时间    2026/5/7
	/////////////////////////////////////////////////////////////////////////////
	int open_database(const QString &db_path);

	/////////////////////////////////////////////////////////////////////////////
	/// @说明
	///          监听 WebSocket 端口
	/// @参数
	///          address--绑定地址；port--端口
	/// @返回值 是否成功
	///
	/// @时间    2026/5/7
	/////////////////////////////////////////////////////////////////////////////
	bool listen(const QHostAddress &address = QHostAddress::Any, quint16 port = 0);

private slots:
	void on_new_connection();
	void on_disconnected();
	void on_web_socket_error(QAbstractSocket::SocketError error);
	void on_binary_message_received(const QByteArray &message);
	void on_text_message_received(const QString &message);

private:
	QWebSocketServer *m_web_socket_server = nullptr;
	ClientRegistry m_client_registry;
	SignalingRelay m_signaling_relay{&m_client_registry};
	signaling_api_handler m_api_handler{&m_client_registry, &m_signaling_relay};
};

#endif
