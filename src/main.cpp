#include "mainwindow.h"
#include <QApplication>
#include <iostream> // [ADAPTRONICS] necessario per std::cout nel message handler

int main(int argc, char *argv[]) {

	// [ADAPTRONICS] BEGIN — redirige tutti i messaggi Qt (qInfo/qWarning/qCritical) su stdout
	// così Python li legge via pipe insieme a RECORDING_STARTED e RECORDING_DONE
	qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &msg) {
		const char *level = "INFO";
		if (type == QtWarningMsg)  level = "WARNING";
		if (type == QtCriticalMsg) level = "CRITICAL";
		if (type == QtFatalMsg)    level = "FATAL";
		std::cout << "[LabRecorder] LOG:" << level << ":" << msg.toStdString() << std::endl;
		std::cout.flush();
		if (type == QtFatalMsg) abort();
	});
	// [ADAPTRONICS] END — message handler

	// determine the startup config file...
	const char *config_file = nullptr;
	for (int k = 1; k < argc; k++)
		if (std::string(argv[k]) == "-c" || std::string(argv[k]) == "--config")
			config_file = argv[k + 1];

	QApplication a(argc, argv);
	MainWindow w(nullptr, config_file);
	w.show();
	return a.exec();
}
