////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 信令中继实现
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "SignalingRelay.h"
#include "ClientRegistry.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtWebSockets/QWebSocket>

namespace {

QString json_bytes_for_console_log(const QByteArray &utf8)
{
	const QString s = QString::fromUtf8(utf8);
	const int max_chars = 4096;
	if (s.size() <= max_chars)
		return s;
	return s.left(max_chars) + QStringLiteral("…(截断，原长 %1 字符)").arg(s.size());
}

} // namespace

SignalingRelay::SignalingRelay(ClientRegistry *client_registry) : m_client_registry(client_registry) {}

bool SignalingRelay::relay_text_message(QWebSocket *from, const QString &message_text) const
{
	if (!m_client_registry || !from)
		return false;

	const auto doc = QJsonDocument::fromJson(message_text.toUtf8());
	if (!doc.isObject())
		return false;

	QJsonObject object = doc.object();
	const QString destination_id = object.value(QStringLiteral("id")).toString();
	if (destination_id.isEmpty())
		return false;

	QWebSocket *destination = m_client_registry->get(destination_id);
	if (!destination)
		return false;

	object[QStringLiteral("id")] = from->objectName();
	const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Compact);
	const QString out = QString::fromUtf8(data);
    qInfo().noquote() << QStringLiteral("[tran]") << from->objectName() << QStringLiteral("->")
	                  << destination->objectName() << json_bytes_for_console_log(data);
	destination->sendTextMessage(out);
	destination->flush();
	return true;
}
