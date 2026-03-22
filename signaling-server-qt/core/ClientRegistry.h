#ifndef CLIENTREGISTRY_H
#define CLIENTREGISTRY_H

#include <QMap>
#include <QString>
#include <QStringList>

class QWebSocket;

/**
 * 注册表模式：按 client id 管理 WebSocket 连接，与信令路由解耦。
 */
class ClientRegistry {
public:
	void add(const QString &id, QWebSocket *socket);
	void remove(const QString &id);
	QWebSocket *get(const QString &id) const;
	bool contains(const QString &id) const;
	QStringList ids() const;

private:
	QMap<QString, QWebSocket *> clients_;
};

#endif
