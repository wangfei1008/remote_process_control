////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 信令服务门面实现
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "SignalingServer.h"
#include "db/manage_db.h"

#include <QDebug>
#include <QString>
#include <QtWebSockets>

namespace {

QString json_text_for_console_log(const QString &json_text)
{
	const int max_chars = 4096;
	if (json_text.size() <= max_chars)
		return json_text;
	return json_text.left(max_chars)
	       + QStringLiteral("…(截断，原长 %1 字符)").arg(json_text.size());
}

} // namespace

SignalingServer::SignalingServer(QObject *parent)
	: QObject(parent)
{
	m_web_socket_server =
	    new QWebSocketServer(QStringLiteral("SignalingServer"), QWebSocketServer::NonSecureMode, this);
	QObject::connect(m_web_socket_server, &QWebSocketServer::newConnection, this,
	                 &SignalingServer::on_new_connection);
}

int SignalingServer::open_database(const QString &db_path)
{
	const int rc = m_api_handler.open_database(db_path);
    if (rc != manage_db_ok)
		qWarning() << "Failed to open database" << db_path << "rc=" << rc;
	return rc;
}

bool SignalingServer::listen(const QHostAddress &address, quint16 port)
{
	return m_web_socket_server->listen(address, port);
}

void SignalingServer::on_new_connection()
{
	auto *web_socket = m_web_socket_server->nextPendingConnection();
	const auto client_id = web_socket->requestUrl().path().split('/').value(1);
	qInfo() << QStringLiteral("Client %1 connected").arg(client_id);

	m_client_registry.add(client_id, web_socket);
	web_socket->setObjectName(client_id);
	QObject::connect(web_socket, &QWebSocket::disconnected, this, &SignalingServer::on_disconnected);
	QObject::connect(web_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this,
	                 &SignalingServer::on_web_socket_error);
	QObject::connect(web_socket, &QWebSocket::binaryMessageReceived, this,
	                 &SignalingServer::on_binary_message_received);
	QObject::connect(web_socket, &QWebSocket::textMessageReceived, this,
	                 &SignalingServer::on_text_message_received);
}

void SignalingServer::on_disconnected()
{
	auto *web_socket = qobject_cast<QWebSocket *>(sender());
	if (web_socket) {
		m_api_handler.on_socket_disconnected(web_socket->objectName());
		m_client_registry.remove(web_socket->objectName());
	}
}

void SignalingServer::on_web_socket_error(QAbstractSocket::SocketError error)
{
	qDebug() << QStringLiteral("Client %1 error %2")
	            .arg(sender()->objectName())
	            .arg(static_cast<int>(error));
}

void SignalingServer::on_binary_message_received(const QByteArray &message)
{
	qInfo() << QStringLiteral("Client %1 binary %2 bytes")
	            .arg(sender()->objectName())
	            .arg(message.size());
}

void SignalingServer::on_text_message_received(const QString &message)
{
	auto *web_socket = qobject_cast<QWebSocket *>(sender());
	if (!web_socket)
		return;

    qInfo().noquote() << QStringLiteral("[req]") << web_socket->objectName()
	                  << json_text_for_console_log(message);

	m_api_handler.handle_text_message(web_socket, message);
}
