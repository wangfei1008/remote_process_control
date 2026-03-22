#include "ClientRegistry.h"
#include <QtWebSockets/QWebSocket>

void ClientRegistry::add(const QString &id, QWebSocket *socket)
{
	clients_[id] = socket;
}

void ClientRegistry::remove(const QString &id)
{
	clients_.remove(id);
}

QWebSocket *ClientRegistry::get(const QString &id) const
{
	return clients_.value(id, nullptr);
}

bool ClientRegistry::contains(const QString &id) const
{
	return clients_.contains(id);
}

QStringList ClientRegistry::ids() const
{
	return clients_.keys();
}
