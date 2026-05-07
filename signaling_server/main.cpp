////////////////////////////////////////////////////////////////////////////////////////////////////////
// 功能说明： 信令服务进程入口（WebSocket + SQLite）
//
// 作者：WangFei
// 时间： 2026-05-07
// 修改:
//              1、2026-05-07创建
//
//详细功能说明：
// - 参数： [host] [port] [db_path]
// - 默认 0.0.0.0:9090，数据库默认 db/manage.db（相对当前工作目录）
//
////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "SignalingServer.h"
#include "db/manage_db.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QStringList>

int main(int argc, char *argv[])
{
	QCoreApplication application(argc, argv);
	SignalingServer server;

	const QStringList args = application.arguments();
	const QString host = args.size() >= 2 ? args.at(1) : QString("0.0.0.0");
	const quint16 port = args.size() >= 3 ? args.at(2).toUShort() : 9090;
	const QString db_path_arg = args.size() >= 4 ? args.at(3) : QStringLiteral("db/manage.db");
	// 相对路径相对「进程当前工作目录」；与手工打开的 db/manage.db 不是同一文件时易误判
	const QFileInfo db_fi(db_path_arg);
	const QString db_abs = db_fi.isAbsolute() ? db_fi.absoluteFilePath() : QDir::current().absoluteFilePath(db_path_arg);

	const int db_rc = server.open_database(db_abs);
	if (db_rc != manage_db_ok)
		qCritical() << "SQLite open failed; admin CRUD and login will not work. rc=" << db_rc;

	if (server.listen(QHostAddress(host), port)) {
		qInfo() << "Listening on" << host << ":" << port;
	} else {
		qDebug() << "Failed to bind on" << host << ":" << port;
	}
	return application.exec();
}
