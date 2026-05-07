////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： WebSocket 客户端登记实现
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "ClientRegistry.h"
#include <QtWebSockets/QWebSocket>

void ClientRegistry::add(const QString &id, QWebSocket *socket)
{
	m_clients[id] = socket;
}

void ClientRegistry::remove(const QString &id)
{
	m_clients.remove(id);
}

QWebSocket *ClientRegistry::get(const QString &id) const
{
	return m_clients.value(id, nullptr);
}

bool ClientRegistry::contains(const QString &id) const
{
	return m_clients.contains(id);
}

QStringList ClientRegistry::ids() const
{
	return m_clients.keys();
}
