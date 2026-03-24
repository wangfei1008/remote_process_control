/*
 * Qt signaling server — WebSocket relay for WebRTC SDP exchange.
 * 架构：SignalingServer（门面） + ClientRegistry（注册表） + SignalingRelay（路由策略）
 */
#ifndef SIGNALINGSERVER_H
#define SIGNALINGSERVER_H

#include <QAbstractSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include "core/ClientRegistry.h"
#include "core/SignalingRelay.h"

class QWebSocketServer;
class QWebSocket;

class SignalingServer : public QObject {
	Q_OBJECT
public:
	explicit SignalingServer(QObject *parent = nullptr);
	bool listen(const QHostAddress &address = QHostAddress::Any, quint16 port = 0);

private slots:
	void on_new_connection();
	void on_disconnected();
	void on_web_socket_error(QAbstractSocket::SocketError error);
	void on_binary_message_received(const QByteArray &message);
	void on_text_message_received(const QString &message);

private:
	QWebSocketServer *server = nullptr;
	ClientRegistry registry_;
	SignalingRelay relay_;
};

#endif
