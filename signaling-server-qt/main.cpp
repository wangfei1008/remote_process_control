/**
 * Qt signaling server example for libdatachannel
 * Copyright (c) 2022 cheungxiongwei
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "SignalingServer.h"
#include <QCoreApplication>
#include <QStringList>

int main(int argc, char *argv[]) {
	QCoreApplication a(argc, argv);
	SignalingServer server;
    const QStringList args = a.arguments();
    const QString host = args.size() >= 2 ? args.at(1) : QString("0.0.0.0");
    const quint16 port = args.size() >= 3 ? args.at(2).toUShort() : 9090;

	if (server.listen(QHostAddress(host), port)) {
		qInfo() << "Listening on" << host << ":" << port;
	} else {
		qDebug() << "Failed to bind on" << host << ":" << port;
	}
	return a.exec();
}
