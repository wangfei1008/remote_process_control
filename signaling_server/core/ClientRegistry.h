////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： WebSocket 客户端登记（按 client_id 索引）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - URL path 中的 /<client_id> 作为键，便于 SignalingRelay 按目标 id 投递
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef CLIENTREGISTRY_H
#define CLIENTREGISTRY_H

#include <QMap>
#include <QString>
#include <QStringList>

class QWebSocket;

class ClientRegistry {
public:
	void add(const QString &id, QWebSocket *socket);
	void remove(const QString &id);
	QWebSocket *get(const QString &id) const;
	bool contains(const QString &id) const;
	QStringList ids() const;

private:
	QMap<QString, QWebSocket *> m_clients;
};

#endif
