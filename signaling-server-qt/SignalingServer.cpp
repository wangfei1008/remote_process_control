/**
 * Qt signaling server — WebSocket relay for WebRTC SDP exchange.
 */
#include "SignalingServer.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtWebSockets>
#include <format>

SignalingServer::SignalingServer(QObject *parent)
	: QObject(parent)
	, relay_(&registry_)
{
	server = new QWebSocketServer(QStringLiteral("SignalingServer"), QWebSocketServer::NonSecureMode, this);
	QObject::connect(server, &QWebSocketServer::newConnection, this, &SignalingServer::on_new_connection);
}

bool SignalingServer::listen(const QHostAddress &address, quint16 port)
{
	return server->listen(address, port);
}

void SignalingServer::on_new_connection()
{
	auto *webSocket = server->nextPendingConnection();
	const auto client_id = webSocket->requestUrl().path().split('/').value(1);
	qInfo() << QString::fromStdString(
	    std::format("Client {} connected", client_id.toUtf8().constData()));

	registry_.add(client_id, webSocket);
	webSocket->setObjectName(client_id);
	QObject::connect(webSocket, &QWebSocket::disconnected, this, &SignalingServer::on_disconnected);
	QObject::connect(webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this,
	                 &SignalingServer::on_web_socket_error);
	QObject::connect(webSocket, &QWebSocket::binaryMessageReceived, this,
	                 &SignalingServer::on_binary_message_received);
	QObject::connect(webSocket, &QWebSocket::textMessageReceived, this,
	                 &SignalingServer::on_text_message_received);
}

void SignalingServer::on_disconnected()
{
	auto *webSocket = qobject_cast<QWebSocket *>(sender());
	if (webSocket)
		registry_.remove(webSocket->objectName());
}

void SignalingServer::on_web_socket_error(QAbstractSocket::SocketError error)
{
	qDebug() << QString::fromStdString(std::format("Client {} << {}",
	                                                 sender()->objectName().toUtf8().constData(),
	                                                 QString::number(error).toUtf8().constData()));
}

void SignalingServer::on_binary_message_received(const QByteArray &message)
{
	qInfo() << QString::fromStdString(std::format(
	    "Client {} << {}", sender()->objectName().toUtf8().constData(), message.constData()));
}

void SignalingServer::on_text_message_received(const QString &message)
{
	auto *webSocket = qobject_cast<QWebSocket *>(sender());
	if (!webSocket)
		return;

	qInfo() << QString::fromStdString(std::format("Client {} << {}",
	                                              webSocket->objectName().toUtf8().constData(),
	                                              message.toUtf8().constData()));

	const auto obj = QJsonDocument::fromJson(message.toUtf8()).object();
	const QString destination_id = obj.value(QStringLiteral("id")).toString();

	if (relay_.relay_text_message(webSocket, message)) {
		qInfo() << QString::fromStdString(
		    std::format("Relay {} -> {}", webSocket->objectName().toUtf8().constData(),
		                destination_id.toUtf8().constData()));
	} else {
		qInfo() << QString::fromStdString(
		    std::format("Destination not found: {}", destination_id.toUtf8().constData()));
	}
}
