#pragma once

#include <cstdint>

#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QDataStream>
#include <qregularexpression.h>

class RemoteControlSocket : public QObject {
	Q_OBJECT
	QTcpServer server;
	QList<QTcpSocket*> clients;
public:
	RemoteControlSocket(uint16_t port);

signals:
	void refresh_streams();
	void start();
	void stop();
	void filename(QString s);
	void select_all();
	void select_none();
	void status_requested(QTcpSocket *sock); // [ADAPTRONICS] comando status: risponde con run corrente

public slots:
	void addClient();
	void handleLine(QString s, QTcpSocket* sock);
};
