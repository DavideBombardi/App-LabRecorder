#include "tcpinterface.h"
#include <QDebug>

RemoteControlSocket::RemoteControlSocket(uint16_t port) : server() {
	server.listen(QHostAddress::Any, port);
	connect(&server, &QTcpServer::newConnection, this, &RemoteControlSocket::addClient);
}

void RemoteControlSocket::addClient() {
	auto *client = server.nextPendingConnection();
	clients.push_back(client);
	connect(client, &QTcpSocket::readyRead, this, [this, client]() {
		while(client->canReadLine())
			this->handleLine(client->readLine().trimmed(), client);
	});
}

void RemoteControlSocket::handleLine(QString s, QTcpSocket *sock) {
	qInfo() << s;
	if (s == "start")
		emit start();
	// [ADAPTRONICS] BEGIN — "stop" risponde OK prima di emettere il segnale, così il flush
	// del file XDF (bloccante) non tiene Python fermo su readline().
	// Python riceve subito OK e poi attende RECORDING_DONE su stdout.
	else if (s == "stop") {
		sock->write("OK\n");
		sock->flush(); // [ADAPTRONICS] forza invio immediato prima che emit stop() blocchi l'event loop
		emit stop();
		return;
	}
	// [ADAPTRONICS] END — stop anticipato
	else if (s == "update")
			emit refresh_streams();
	else if (s.contains("filename")) {
		emit filename(s);
	} else if (s.contains("select")) {
		if (s.contains("all")) {
			emit select_all();
		} else if (s.contains("none")) {
			emit select_none();
		// [ADAPTRONICS] BEGIN — selezione stream specifici per nome+host
		} else if (s.contains("{stream:")) {
			emit select_streams(s);
		}
		// [ADAPTRONICS] END — selezione stream specifici
	// [ADAPTRONICS] BEGIN — comando status: restituisce run corrente come JSON invece di OK
	} else if (s == "status") {
		emit status_requested(sock);
		return; // la risposta la scrive MainWindow tramite il segnale, non scriviamo OK
	}
	// [ADAPTRONICS] END — comando status
	sock->write("OK\n"); // [ADAPTRONICS] aggiunto \n per permettere readline() lato Python
	// TODO: select /deselect streams
	// TODO: send acknowledgement
	// TODO: get current state
	//
	// else this->sender()->sender("Whoops");
}
