#include "SignalingRelay.h"
#include "ClientRegistry.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QtWebSockets/QWebSocket>

SignalingRelay::SignalingRelay(ClientRegistry *registry) : registry_(registry) {}

bool SignalingRelay::relayTextMessage(QWebSocket *from, const QString &messageText) const
{
	if (!registry_ || !from)
		return false;

	const auto doc = QJsonDocument::fromJson(messageText.toUtf8());
	if (!doc.isObject())
		return false;

	QJsonObject obj = doc.object();
	const QString destinationId = obj.value(QStringLiteral("id")).toString();
	if (destinationId.isEmpty())
		return false;

	QWebSocket *destination = registry_->get(destinationId);
	if (!destination)
		return false;

	obj[QStringLiteral("id")] = from->objectName();
	const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
	destination->sendTextMessage(QString::fromUtf8(data));
	destination->flush();
	return true;
}
