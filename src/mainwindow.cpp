#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDateTime>
#include <QDebug>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QCompleter>    // [ADAPTRONICS] necessario per setCaseSensitivity sui QComboBox
#include <QPlainTextEdit> // [ADAPTRONICS] necessario per il campo Note multilinea
#include <QTextStream>   // [ADAPTRONICS] necessario per leggere LR_Runtime_Entries.csv
#include <QSysInfo>      // [ADAPTRONICS] necessario per QSysInfo::machineHostName()
#include <QDialog>       // [ADAPTRONICS] necessario per il popup note post-test
#include <QVBoxLayout>   // [ADAPTRONICS] necessario per il popup note post-test
#include <QPushButton>   // [ADAPTRONICS] necessario per il popup note post-test
#if QT_VERSION_MAJOR < 6
#include <QRegExp>
#else
#include <QRegularExpression>
using QRegExp = QRegularExpression;
#endif

#include <iostream>  // [ADAPTRONICS] std::cout per notifica completamento registrazione
#include <string>
#include <vector>

// recording class
#include "recording.h"
#include "tcpinterface.h"

const QStringList bids_modalities_default = QStringList({"eeg", "ieeg", "meg", "beh"});

MainWindow::MainWindow(QWidget *parent, const char *config_file)
	: QMainWindow(parent), ui(new Ui::MainWindow) {
	ui->setupUi(this);
	connect(ui->actionLoad_Configuration, &QAction::triggered, this, [this]() {
		load_config(QFileDialog::getOpenFileName(
			this, "Load Configuration File", "", "Configuration Files (*.cfg)"));
	});
	connect(ui->actionSave_Configuration, &QAction::triggered, this, [this]() {
		save_config(QFileDialog::getSaveFileName(
			this, "Save Configuration File", "", "Configuration Files (*.cfg)"));
	});
	connect(ui->actionQuit, &QAction::triggered, this, &MainWindow::close);

	// Signals for stream finding/selecting/starting/stopping
	connect(ui->refreshButton, &QPushButton::clicked, this, &MainWindow::refreshStreams);
	connect(ui->selectAllButton, &QPushButton::clicked, this, &MainWindow::selectAllStreams);
	connect(ui->selectNoneButton, &QPushButton::clicked, this, &MainWindow::selectNoStreams);
	connect(ui->startButton, &QPushButton::clicked, this, &MainWindow::startRecording);
	connect(ui->stopButton, &QPushButton::clicked, this, &MainWindow::stopRecording);
	connect(ui->actionAbout, &QAction::triggered, this, [this]() {
		QString infostr = QStringLiteral("LSL library version: ") +
						  QString::number(lsl::library_version()) +
						  "\nLSL library info:" + lsl::library_info();
		QMessageBox::about(this, "About this app", infostr);
	});

    // Signals for Remote Control Socket
	connect(ui->rcsCheckBox, &QCheckBox::toggled, this, &MainWindow::rcsCheckBoxChanged);
	connect(ui->rcsport, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::rcsportValueChangedInt);

	// Wheenver lineEdit_template is changed, print the final result.
	connect(
		ui->lineEdit_template, &QLineEdit::textChanged, this, &MainWindow::printReplacedFilename);
	auto spinchanged = static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged);
	connect(ui->spin_counter, spinchanged, this, &MainWindow::printReplacedFilename);

	// Signals for builder-related edits -> buildFilename
	connect(ui->rootBrowseButton, &QPushButton::clicked, this, [this]() {
		this->ui->rootEdit->setText(QDir::toNativeSeparators(
			QFileDialog::getExistingDirectory(this, "Study root folder...")));
		this->buildFilename();
	});
	connect(ui->rootEdit, &QLineEdit::editingFinished, this, &MainWindow::buildFilename);
	// [ADAPTRONICS] segnale cambiato da QLineEdit::editingFinished a QComboBox::currentTextChanged
	//               perché i tre campi sono stati convertiti in QComboBox editabili
	connect(ui->lineEdit_participant, &QComboBox::currentTextChanged, this, &MainWindow::buildFilename);
	connect(ui->lineEdit_session, &QComboBox::currentTextChanged, this, &MainWindow::buildFilename);
	connect(ui->lineEdit_acq, &QComboBox::currentTextChanged, this, &MainWindow::buildFilename);

	// [ADAPTRONICS] tendina a cascata: CAD ID → ID Patch
	// quando CAD ID cambia: ripopola ID Patch con i figli del CAD selezionato
	connect(ui->lineEdit_acq, &QComboBox::currentTextChanged, this, [this](const QString &text) {
		ui->lineEdit_participant->clear();
		ui->lineEdit_participant->addItems(atFilteredList("PATCH:" + text));
		if (ui->lineEdit_participant->completer())
			ui->lineEdit_participant->completer()->setCaseSensitivity(Qt::CaseInsensitive);
	});
	// [FINE ADAPTRONICS] tendina a cascata
	connect(ui->input_blocktask, &QComboBox::currentTextChanged, this, &MainWindow::buildFilename);
	connect(ui->input_modality, &QComboBox::currentTextChanged, this, &MainWindow::buildFilename);
	connect(ui->check_bids, &QCheckBox::toggled, this, [this](bool checked) {
		auto &box = *ui->lineEdit_template;
		box.setReadOnly(checked);
		if (checked) {
			legacyTemplate = box.text();
			box.setText(QDir::toNativeSeparators(
				QStringLiteral("sub-%p/ses-%s/%m/sub-%p_ses-%s_task-%b[_acq-%a]_run-%r_%m.xdf")));
			ui->label_counter->setText("Run (%r)");
		} else {
			box.setText(QDir::toNativeSeparators(legacyTemplate));
			ui->label_counter->setText("Exp num (%n)");
		}
	});

	timer = std::make_unique<QTimer>(this);
	connect(&*timer, &QTimer::timeout, this, &MainWindow::statusUpdate);
	timer->start(1000);

	QString cfgfilepath = find_config_file(config_file);
	load_config(cfgfilepath);
}

void MainWindow::statusUpdate() const {
	if (currentRecording) {
		auto elapsed = static_cast<int>(lsl::local_clock() - startTime);
		QString recFilename = replaceFilename(QDir::cleanPath(ui->lineEdit_template->text()));
		auto fileinfo = QFileInfo(QDir::cleanPath(ui->rootEdit->text()) + '/' + recFilename);
		fileinfo.refresh();
		auto size = fileinfo.size();
		QString timeString = QStringLiteral("Recording to %1 (%2; %3kb)")
								 .arg(QDir::toNativeSeparators(recFilename),
									 QTime(0,0).addSecs(elapsed).toString("hh:mm:ss"),
									 QString::number(size / 1000));
		statusBar()->showMessage(timeString);
	}
}

void MainWindow::closeEvent(QCloseEvent *ev) {
	if (currentRecording) ev->ignore();
}

void MainWindow::blockSelected(const QString &block) {
	if (currentRecording)
		QMessageBox::information(this, "Still recording",
			"Please stop recording before switching blocks.", QMessageBox::Ok);
	else {
		printReplacedFilename();
		// scripted action code here...
	}
}

void MainWindow::load_config(QString filename) {
	qInfo() << "loading config file " << QDir::toNativeSeparators(filename);
	bool auto_start = false;
	try {
		QSettings pt(QDir::cleanPath(filename), QSettings::Format::IniFormat);

		// ----------------------------
		// required streams
		// ----------------------------
		auto required = pt.value("RequiredStreams").toStringList();
#if QT_VERSION >= QT_VERSION_CHECK(5,14,0)
		missingStreams = QSet<QString>(required.begin(), required.end());
#else
		missingStreams = required.toSet();
#endif

		// ----------------------------
		// online sync streams
		// ----------------------------
		QStringList onlineSyncStreams = pt.value("OnlineSync", QStringList()).toStringList();
		for (QString &oss : onlineSyncStreams) {
#if QT_VERSION >= QT_VERSION_CHECK(5,14,0)
			auto skipEmpty = Qt::SkipEmptyParts;
#else
			auto skipEmpty = QString::SkipEmptyParts;
#endif

			QStringList words = oss.split(' ', skipEmpty);
			// The first two words ("StreamName (PC)") are the stream identifier
			if (words.length() < 2) {
				qInfo() << "Invalid sync stream config: " << oss;
				continue;
			}
			QString key = words.takeFirst() + ' ' + words.takeFirst();

			int val = 0;
			for (const auto &word : std::as_const(words)) {
				if (word == "post_clocksync") { val |= lsl::post_clocksync; }
				if (word == "post_dejitter") { val |= lsl::post_dejitter; }
				if (word == "post_monotonize") { val |= lsl::post_monotonize; }
				if (word == "post_threadsafe") { val |= lsl::post_threadsafe; }
				if (word == "post_ALL") { val = lsl::post_ALL; }
			}
			syncOptionsByStreamName[key.toStdString()] = val;
			qInfo() << "stream sync options: " << key << ": " << val;
		}

		// ----------------------------
		// Block/Task Names
		// ----------------------------
		QStringList taskNames;
		if (pt.contains("SessionBlocks")) { taskNames = pt.value("SessionBlocks").toStringList(); }
		ui->input_blocktask->clear();
		ui->input_blocktask->insertItems(0, taskNames);

		// StorageLocation
		QString studyRoot;
		legacyTemplate.clear();

		if (pt.contains("StorageLocation")) {
			if (pt.contains("StudyRoot"))
				throw std::runtime_error("StorageLocation cannot be used if StudyRoot is also specified.");
			if (pt.contains("PathTemplate"))
				throw std::runtime_error("StorageLocation cannot be used if PathTemplate is also specified.");

			QString str_path = pt.value("StorageLocation").toString();
			QString path_root;
			auto index = str_path.indexOf('%');
			if (index != -1) {
				// When a % is encountered, the studyroot gets set to the
				// longest path before the placeholder, e.g.
				// foo/bar/baz%a/untitled.xdf gets split into
				// foo/bar and baz%a/untitled.xdf
				path_root = str_path.left(index);
			} else {
				// Otherwise, it's split into folder and constant filename
				path_root = str_path;
			}
			studyRoot = QFileInfo(path_root).absolutePath();
			legacyTemplate = str_path.remove(0, studyRoot.length() + 1);
			// absolute path, nothing to be done
			// studyRoot = QFileInfo(path_root).absolutePath();
		}
		// StudyRoot
		if (pt.contains("StudyRoot")) { studyRoot = pt.value("StudyRoot").toString(); }
		if (pt.contains("PathTemplate")) { legacyTemplate = pt.value("PathTemplate").toString(); }

		if (studyRoot.isEmpty())
			studyRoot = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
						QDir::separator() + "CurrentStudy";
		ui->rootEdit->setText(QDir::toNativeSeparators(studyRoot));

		if (legacyTemplate.isEmpty()) {
			ui->check_bids->setChecked(true);
			// Use default, exp%n/%b.xdf , only to be used if BIDS gets unchecked.
			legacyTemplate = "exp%n/block_%b.xdf";
		} else {
			ui->check_bids->setChecked(false);
			ui->lineEdit_template->setText(QDir::toNativeSeparators(legacyTemplate));
		}

		// Append BIDS modalities to the default list.
		ui->input_modality->insertItems(
			ui->input_modality->count(), pt.value("BidsModalities", bids_modalities_default).toStringList());
		ui->input_modality->setCurrentIndex(0);

		buildFilename();

		// Remote Control Socket options
		if (pt.contains("RCSPort")) {
			int rcs_port = pt.value("RCSPort").toInt();
			ui->rcsport->setValue(rcs_port);
			// In case it's already running (how?), stop the RCS listener.
			ui->rcsCheckBox->setChecked(false);
		}

		if (pt.contains("RCSEnabled")) {
			bool b_enable_rcs = pt.value("RCSEnabled").toBool();
			ui->rcsCheckBox->setChecked(b_enable_rcs);
		}

		// Check the wild-card-replaced filename to see if it exists already.
		// If it does then increment the exp number.
		// We only do this on settings-load because manual spin changes might indicate purposeful
		// overwriting.
		QString recFilename = QDir::cleanPath(ui->lineEdit_template->text());
		// Spin Number
		if (recFilename.contains(counterPlaceholder())) {
			for (int i = 1; i < 1001; i++) {
				ui->spin_counter->setValue(i);
				if (!QFileInfo::exists(replaceFilename(recFilename))) break;
			}
		}

		if (pt.contains("AutoStart")) {
			auto_start = pt.value("AutoStart").toBool();
		}

	} catch (std::exception &e) { qWarning() << "Problem parsing config file: " << e.what(); }

	// [ADAPTRONICS] carica il CSV per le tendine a cascata dalla stessa cartella del .cfg
	QString csvDir = filename.isEmpty()
		? QFileInfo(QCoreApplication::applicationFilePath()).absolutePath()
		: QFileInfo(filename).absolutePath();
	loadAtCsv(csvDir);

	refreshStreams();

	if (auto_start) { startRecording(); }
 }

void MainWindow::save_config(QString filename) {
	QSettings settings(filename, QSettings::Format::IniFormat);
	settings.setValue("StudyRoot", QDir::cleanPath(ui->rootEdit->text()));
	if (!ui->check_bids->isChecked())
		settings.setValue("PathTemplate", QDir::cleanPath(ui->lineEdit_template->text()));
	// Build QStringList from missingStreams and knownStreams that are missing.
	QStringList requiredStreams = missingStreams.values();
	for (auto &k : knownStreams) {
		if (k.checked) { requiredStreams.append(k.listName()); }
	}
	qInfo() << missingStreams;
	settings.setValue("RequiredStreams", requiredStreams);
	// Stub.
}

QString info_to_listName(const lsl::stream_info& info) {
	return QString::fromStdString(info.name() + " (" + info.hostname() + ")");
}

/**
 * @brief MainWindow::refreshStreams Find streams, generate a list of missing streams
 * and fill the UI streamlist.
 * @return A vector of found stream_infos
 */
std::vector<lsl::stream_info> MainWindow::refreshStreams() {
	const std::vector<lsl::stream_info> resolvedStreams = lsl::resolve_streams(1.0);

	// For each item in resolvedStreams, ignore if already in knownStreams, otherwise add to knownStreams.
	// if in missingStreams then also mark it as required (--> checked by default) and remove from missingStreams.
	for (const auto& s : resolvedStreams) {
		bool known = false;
		for (auto &k : knownStreams) {
			known |= s.name() == k.name && s.type() == k.type && s.source_id() == k.id;
		}
		if (!known) {
			bool found = missingStreams.contains(info_to_listName(s));
			knownStreams << StreamItem(s.name(), s.type(), s.source_id(), s.hostname(), found);
			if (found) { missingStreams.remove(info_to_listName(s)); }
		}
	}
	// For each item in knownStreams, update its checked status from GUI. (only works for streams found on a previous refresh)
	// Because we search by name + host, entries aren't guaranteed to be unique, so checking one entry with matching name and host checks them all.
	for (auto &k : knownStreams) {
		QList<QListWidgetItem *> foundItems = ui->streamList->findItems(k.listName(), Qt::MatchCaseSensitive);
		if (foundItems.count() > 0) {
			bool checked = false;
			for (auto &fi : foundItems) { checked |= fi->checkState() == Qt::Checked; }
			k.checked = checked;
		}
	}
	// For each item in knownStreams; if it is not resolved then drop it. If it was checked then add back to missingStreams.
	int k_ind = 0;
	while (k_ind < knownStreams.count()) {
		StreamItem k = knownStreams.at(k_ind);
		bool resolved = false;
		size_t r_ind = 0;
		while (!resolved && r_ind < resolvedStreams.size()) {
			const lsl::stream_info r = resolvedStreams[r_ind];
			resolved |= (r.name() == k.name) && (r.type() == k.type) && (r.source_id() == k.id);
			r_ind++;
		}
		if (!resolved) {
			if (k.checked) { missingStreams += k.listName(); }
			knownStreams.removeAt(k_ind);
		} else {
			k_ind++;
		}
	}
	// Clear the streamList
	// Add missing items first.
	// Then add knownStreams (only in list if resolved).
	const QBrush good_brush(QColor(0, 128, 0)), bad_brush(QColor(255, 0, 0));
	ui->streamList->clear();
	for (auto& m : std::as_const(missingStreams)) {
		auto *item = new QListWidgetItem(m, ui->streamList);
		item->setCheckState(Qt::Checked);
		item->setForeground(bad_brush);
		ui->streamList->addItem(item);
	}
	for (auto& k : knownStreams) {
		auto *item = new QListWidgetItem(k.listName(), ui->streamList);
		item->setCheckState(k.checked ? Qt::Checked : Qt::Unchecked);
		item->setForeground(good_brush);
	    item->setToolTip(QString("Name: %1\nType: %2\nSource ID: %3\nHostname: %4")
            .arg(QString::fromStdString(k.name),
                 QString::fromStdString(k.type),
                 QString::fromStdString(k.id),
                 QString::fromStdString(k.host)));
		ui->streamList->addItem(item);
	}

	// return a std::vector of streams of checked and not missing streams.
	std::vector<lsl::stream_info> requestedAndAvailableStreams;
	for (const auto &r : resolvedStreams) {
		for (auto &k : knownStreams) {
			if ((r.name() == k.name) && (r.type() == k.type) && (r.source_id() == k.id)) {
				if (k.checked) { requestedAndAvailableStreams.push_back(r); }
				break;
			}
		}
	}
	return requestedAndAvailableStreams;
}

void MainWindow::startRecording() {
	if (!currentRecording) {

		// automatically refresh streams
		const std::vector<lsl::stream_info> requestedAndAvailableStreams = refreshStreams();

		if (!hideWarnings) {
			// if a checked stream is now missing
			if (!missingStreams.isEmpty()) {
				// are you sure?
				QMessageBox msgBox(QMessageBox::Warning, "Stream not found",
					"At least one of the streams that you checked seems to be offline",
					QMessageBox::Yes | QMessageBox::No, this);
				msgBox.setInformativeText("Do you want to start recording anyway?");
				msgBox.setDefaultButton(QMessageBox::No);
				if (msgBox.exec() != QMessageBox::Yes) return;
			}

			if (requestedAndAvailableStreams.size() == 0) {
				QMessageBox msgBox(QMessageBox::Warning, "No available streams selected",
					"You have selected no streams", QMessageBox::Yes | QMessageBox::No, this);
				msgBox.setInformativeText("Do you want to start recording anyway?");
				msgBox.setDefaultButton(QMessageBox::No);
				if (msgBox.exec() != QMessageBox::Yes) return;
			}
		}

		// don't hide critical errors.
		QString recFilename = replaceFilename(QDir::cleanPath(ui->lineEdit_template->text()));
		if (recFilename.isEmpty()) {
			QMessageBox::critical(this, "Filename empty", "Can not record without a file name");
			return;
		}
		if (ui->rootEdit->text().trimmed().isEmpty()) {
			QMessageBox::critical(this, "Study Root empty",
				"Can not record without a Study Root folder. "
				"Please set a Study Root before recording.");
			return;
		}
		// [ADAPTRONICS] BEGIN — popup di conferma con riepilogo metadati prima di avviare la registrazione
		{
			QString summary;
			summary += "ID CAD:      " + ui->lineEdit_acq->currentText() + "\n";
			summary += "ID Patch:    " + ui->lineEdit_participant->currentText() + "\n";
			summary += "Operator:    " + ui->comboBox_meta_operator->currentText() + "\n";
			summary += "Material ID: " + ui->comboBox_meta_material->currentText() + "\n";
			summary += "Test ID:     " + ui->comboBox_meta_test->currentText() + "\n";
			summary += "Perno mat.:  " + ui->comboBox_meta_perno_materiale->currentText() + "\n";
			summary += "Perno diam.: " + ui->comboBox_meta_perno_diametro->currentText() + "\n";
			summary += "Perno n.:    " + ui->comboBox_meta_perno_numero->currentText() + "\n";
			summary += "Perno pos.:  " + ui->comboBox_meta_perno_posizione->currentText() + "\n";
			QString note = ui->plainTextEdit_meta_note->toPlainText().trimmed();
			if (!note.isEmpty()) summary += "Note:        " + note + "\n";
			summary += "\nFile: " + QDir::cleanPath(ui->rootEdit->text() + '/' + recFilename);
			QMessageBox confirm(QMessageBox::Question, "Conferma registrazione", summary,
				QMessageBox::Ok | QMessageBox::Cancel, this);
			confirm.button(QMessageBox::Ok)->setText("Conferma e Avvia");
			confirm.button(QMessageBox::Cancel)->setText("Annulla");
			if (confirm.exec() != QMessageBox::Ok) return;
		}
		// [ADAPTRONICS] END — popup conferma

		recFilename.prepend(QDir::cleanPath(ui->rootEdit->text()) + '/');

		QFileInfo recFileInfo(recFilename);
		if (recFileInfo.exists()) {
			if (recFileInfo.isDir()) {
				QMessageBox::warning(
					this, "Error", "Recording path already exists and is a directory");
				return;
			}
			QString rename_to = recFileInfo.absolutePath() + '/' + recFileInfo.baseName() +
								"_old%1." + recFileInfo.suffix();
			// search for highest _oldN
			int i = 1;
			while (QFileInfo::exists(rename_to.arg(i))) i++;
			QString newname = rename_to.arg(i);
			if (!QFile::rename(recFileInfo.absoluteFilePath(), newname)) {
				QMessageBox::warning(this, "Permissions issue",
					"Cannot rename the file " + recFilename + " to " + newname);
				return;
			}
			qInfo() << "Moved existing file to " << newname;
			recFileInfo.refresh();
		}

		// regardless, we need to create the directory if it doesn't exist
		if (!recFileInfo.dir().mkpath(".")) {
			QMessageBox::warning(this, "Permissions issue",
				"Can not create the directory " + recFileInfo.dir().path() +
					". Please check your permissions.");
			return;
		}

		std::vector<std::string> watchfor;
		for (const QString &missing : std::as_const(missingStreams)) {
            std::string query;
			// Convert missing to query expected by lsl::resolve_stream
			// name='BioSemi' and hostname=AASDFSDF
			QRegularExpression re("(.+)\\s+\\((\\S+)\\)");
            QRegularExpressionMatch match = re.match(missing);
            if (match.hasMatch())
            {
                QString name = match.captured(1);
                QString host = match.captured(2);
                query = "name='" + match.captured(1).toStdString() + "'";
                if (host.size() > 1) {
                    query += " and hostname='" + host.toStdString() + "'";
                }
            } else {
                // Regexp failed but we can try using the entire string as the stream name.
                query = "name='" + missing.toStdString() + "'";
            }
			watchfor.push_back(query);
		}
		qInfo() << "Missing: " << missingStreams;

		// [ADAPTRONICS] BEGIN — raccolta metadati sessione dai widget dell'interfaccia
		//   I valori vengono passati a recording → XDFWriter e scritti nella FileHeader XDF
		//   nel blocco <adaptronics>, leggibile da qualsiasi parser XDF (MNE, EEGLAB, ecc.)
		std::map<std::string, std::string> sessionMetadata;
		sessionMetadata["cad_id"]      = ui->lineEdit_acq->currentText().toStdString();
		sessionMetadata["patch_id"]    = ui->lineEdit_participant->currentText().toStdString();
		sessionMetadata["operator"]    = ui->comboBox_meta_operator->currentText().toStdString();
		sessionMetadata["material_id"] = ui->comboBox_meta_material->currentText().toStdString();
		sessionMetadata["test_id"]     = ui->comboBox_meta_test->currentText().toStdString();
		sessionMetadata["note"]              = ui->plainTextEdit_meta_note->toPlainText().toStdString(); // [ADAPTRONICS] QPlainTextEdit → toPlainText()
		sessionMetadata["perno_materiale"]   = ui->comboBox_meta_perno_materiale->currentText().toStdString();
		sessionMetadata["perno_diametro"]    = ui->comboBox_meta_perno_diametro->currentText().toStdString();
		sessionMetadata["perno_numero"]      = ui->comboBox_meta_perno_numero->currentText().toStdString();
		sessionMetadata["perno_posizione"]   = ui->comboBox_meta_perno_posizione->currentText().toStdString();
		sessionMetadata["start_time"]        = QDateTime::currentDateTime().toString(Qt::ISODate).toStdString();
		// [ADAPTRONICS] salva path e metadati per la notifica di completamento in stopRecording()
		lastRecFilename_     = recFilename;
		lastSessionMetadata_ = sessionMetadata;
		// [ADAPTRONICS] END — raccolta metadati sessione

		currentRecording = std::make_unique<recording>(recFilename.toStdString(),
			requestedAndAvailableStreams, watchfor, syncOptionsByStreamName, true, sessionMetadata);
		ui->stopButton->setEnabled(true);
		ui->groupBox_metadata->setEnabled(false); // [ADAPTRONICS] freeze metadati durante la registrazione
		ui->startButton->setEnabled(false);
		startTime = (int)lsl::local_clock();

	} else if (!hideWarnings) {
		QMessageBox::information(
			this, "Already recording", "The recording is already running", QMessageBox::Ok);
	}
}

void MainWindow::stopRecording() {

	if (currentRecording) {
		try {
			currentRecording = nullptr;
		} catch (std::exception &e) { qWarning() << "exception on stop: " << e.what(); }
		ui->startButton->setEnabled(true);
		ui->stopButton->setEnabled(false);
		ui->groupBox_metadata->setEnabled(true); // [ADAPTRONICS] unfreeze metadati dopo la registrazione
		statusBar()->showMessage("Stopped");
		// [ADAPTRONICS] BEGIN — popup note post-test
		// XDF già chiuso — le note vanno solo nel JSON/stdout
		// sia "Chiudi" che la X del popup portano allo stesso punto: il JSON viene sempre inviato
		if (!lastRecFilename_.isEmpty()) {
			QDialog notesDlg(this);
			notesDlg.setWindowTitle("Note post-test");
			notesDlg.setMinimumWidth(420);
			auto *layout = new QVBoxLayout(&notesDlg);
			auto *noteEdit = new QPlainTextEdit(&notesDlg);
			noteEdit->setPlaceholderText("Niente da dichiarare");
			noteEdit->setMinimumHeight(100);
			auto *closeBtn = new QPushButton("Chiudi", &notesDlg);
			connect(closeBtn, &QPushButton::clicked, &notesDlg, &QDialog::accept);
			layout->addWidget(noteEdit);
			layout->addWidget(closeBtn);
			notesDlg.exec(); // ritorna sia su Chiudi che su X — il JSON viene sempre inviato
			lastSessionMetadata_["note_post"] = noteEdit->toPlainText().trimmed().toStdString();
		}
		// [ADAPTRONICS] END — popup note post-test

		// [ADAPTRONICS] notifica completamento: stdout (primario) + last_recording.json (fallback)
		if (!lastRecFilename_.isEmpty()) {
			QString cfgDir = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
			notifyRecordingDone(cfgDir);
		}
		// [ADAPTRONICS] auto-incrementa il run counter dopo ogni stop
		ui->spin_counter->setValue(ui->spin_counter->value() + 1);
	} else if (!hideWarnings) {
		QMessageBox::information(
			this, "Not recording", "There is not ongoing recording", QMessageBox::Ok);
	}
}

void MainWindow::selectAllStreams() {
	for (int i = 0; i < ui->streamList->count(); i++) {
		QListWidgetItem *item = ui->streamList->item(i);
		item->setCheckState(Qt::Checked);
	}
}

void MainWindow::selectNoStreams() {
	for (int i = 0; i < ui->streamList->count(); i++) {
		QListWidgetItem *item = ui->streamList->item(i);
		item->setCheckState(Qt::Unchecked);
	}
}

void MainWindow::buildBidsTemplate() {
	// path/to/CurrentStudy/sub-%p/ses-%s/eeg/sub-%p_ses-%s_task-%b[_acq-%a]_run-%r_eeg.xdf

	// Make sure the BIDS required fields are full.
	// [ADAPTRONICS] .text()/.setText() → .currentText()/.setCurrentText() per QComboBox
	if (ui->lineEdit_participant->currentText().isEmpty()) { ui->lineEdit_participant->setCurrentText("P001"); }
	if (ui->lineEdit_session->currentText().isEmpty()) { ui->lineEdit_session->setCurrentText("S001"); }
	if (ui->input_blocktask->currentText().isEmpty()) {
		ui->input_blocktask->setCurrentText("Default");
	}
	// BIDS modality selection
	if (ui->input_modality->currentText().isEmpty()) {
		ui->input_modality->insertItems(0, bids_modalities_default);
		ui->input_modality->setCurrentIndex(0);
	}

	// Folder hierarchy
	QStringList fileparts{"sub-%p", "ses-%s", "%m"};

	// filename
	QString fname = "sub-%p_ses-%s_task-%b";
	if (!ui->lineEdit_acq->currentText().isEmpty()) { fname.append("_acq-%a"); } // [ADAPTRONICS] .text() → .currentText() (QComboBox)
	fname.append("_run-%r_%m.xdf");
	fileparts << fname;
	ui->lineEdit_template->setText(QDir::toNativeSeparators(fileparts.join('/')));
}

void MainWindow::buildFilename() {
	// This function is only called when a widget within Location Builder is activated.

	// Build the file location in parts, starting with the root folder.
	if (ui->check_bids->isChecked()) { buildBidsTemplate(); }
	QString tpl = QDir::cleanPath(ui->lineEdit_template->text());

	// Auto-increment Spin/Run Number if necessary.
	// [ADAPTRONICS] prepend StudyRoot al path: senza di esso QFileInfo::exists() controlla
	// un path relativo che non esiste mai, lasciando il counter bloccato a 1
	if (tpl.contains(counterPlaceholder())) {
		QString fullTpl = QDir::cleanPath(ui->rootEdit->text() + '/' + tpl);
		for (int i = 1; i < 1001; i++) {
			ui->spin_counter->setValue(i);
			if (!QFileInfo::exists(replaceFilename(fullTpl))) break;
		}
	}
	// Sometimes lineEdit_template doesn't change so printReplacedFilename isn't triggered.
	// So trigger manually.
	printReplacedFilename();
}

QString MainWindow::replaceFilename(QString fullfile) const {
	// Replace wildcards.
	// There are two different wildcard formats: legacy, BIDS

	// Legacy takes the form path/to/study/exp%n/%b.xdf
	// Where %n will be replaced by the contents of the spin_counter widget
	// and %b will be replaced by the contents of the blockList widget.
	fullfile.replace("%b", ui->input_blocktask->currentText());

	// BIDS
	// See https://docs.google.com/document/d/1ArMZ9Y_quTKXC-jNXZksnedK2VHHoKP3HCeO5HPcgLE/
	// path/to/study/sub-<participant_label>/ses-<session_label>/eeg/sub-<participant_label>_ses-<session_label>_task-<task_label>[_acq-<acq_label>]_run-<run_index>_eeg.xdf
	// path/to/study/sub-%p/ses-%s/eeg/sub-%p_ses-%s_task-%b[_acq-%a]_run-%r_eeg.xdf
	// %b already replaced above.
	// [ADAPTRONICS] .text() → .currentText() perché i campi sono ora QComboBox editabili
	fullfile.replace("%p", ui->lineEdit_participant->currentText());
	fullfile.replace("%s", ui->lineEdit_session->currentText());
	fullfile.replace("%a", ui->lineEdit_acq->currentText());
	fullfile.replace("%m", ui->input_modality->currentText());

	// Replace either %r or %n with the counter
	QString run = QString("%1").arg(ui->spin_counter->value(), 3, 10, QChar('0'));
	fullfile.replace(counterPlaceholder(), run);

	return fullfile.trimmed();
}

/**
 * Find a config file to load. This is (in descending order or preference):
 * - a file supplied on the command line
 * - [executablename].cfg in one the the following folders:
 *	- the current working directory
 *	- the default config folder, e.g. '~/Library/Preferences' on OS X
 *	- the executable folder
 * @param filename	Optional file name supplied e.g. as command line parameter
 * @return Path to a found config file
 */
QString MainWindow::find_config_file(const char *filename) {
	if (filename) {
		QString qfilename(filename);
		if (!QFileInfo::exists(qfilename))
			QMessageBox(QMessageBox::Warning, "Config file not found",
				QStringLiteral("The file '%1' doesn't exist").arg(qfilename), QMessageBox::Ok,
				this);
		else
			return qfilename;
	}
	QFileInfo exeInfo(QCoreApplication::applicationFilePath());
	QString defaultCfgFilename(exeInfo.completeBaseName() + ".cfg");
	qInfo() << defaultCfgFilename;
	QStringList cfgpaths;
	cfgpaths << QDir::currentPath()
			 << QStandardPaths::standardLocations(QStandardPaths::AppConfigLocation)
			 << QStandardPaths::standardLocations(QStandardPaths::AppDataLocation)
			 << exeInfo.path();
	for (const auto &path : std::as_const(cfgpaths)) {
		QString cfgfilepath = path + QDir::separator() + defaultCfgFilename;
		qInfo() << cfgfilepath;
		if (QFileInfo::exists(cfgfilepath)) return cfgfilepath;
	}
    QMessageBox msgBox;
    msgBox.setWindowTitle("Config file not found");
    msgBox.setText("Config file not found.");
    msgBox.setInformativeText("Continuing with default config.");
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();
	return "";
}

QString MainWindow::counterPlaceholder() const { return ui->check_bids->isChecked() ? "%r" : "%n"; }

void MainWindow::printReplacedFilename() {
	ui->locationLabel->setText(
		ui->rootEdit->text() + '\n' + replaceFilename(ui->lineEdit_template->text()));
}

// [ADAPTRONICS] assembla il JSON di completamento registrazione
// formato: {"status":"done","path":"...","cad_id":"...","patch_id":"...",...}
QString MainWindow::buildCompletionJson() const {
	QString json = "{";
	json += "\"status\":\"done\",";
	json += "\"path\":\"" + QString(lastRecFilename_).replace('\\', '/') + "\"";
	for (const auto &kv : lastSessionMetadata_) {
		QString key   = QString::fromStdString(kv.first);
		QString value = QString::fromStdString(kv.second)
			.replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n");
		json += ",\"" + key + "\":\"" + value + "\"";
	}
	json += ",\"end_time\":\"" + QDateTime::currentDateTime().toString(Qt::ISODate) + "\"";
	json += ",\"hostname\":\"" + QSysInfo::machineHostName() + "\"";
	json += "}";
	return json;
}

// [ADAPTRONICS] notifica il completamento della registrazione:
//   1. stdout  → letto dalla GUI Python via pipe (primario)
//   2. file JSON → fallback garantito su disco nella stessa cartella del cfg
void MainWindow::notifyRecordingDone(const QString &cfgDir) const {
	const QString json = buildCompletionJson();

	// 1. stdout
	std::cout << "[LabRecorder] RECORDING_DONE:" << json.toStdString() << std::endl;
	std::cout.flush();

	// 2. fallback file JSON
	QString jsonPath = cfgDir + "/last_recording.json";
	QFile f(jsonPath);
	if (f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		QTextStream out(&f);
		out << json << "\n";
	}
}

// [ADAPTRONICS] helper: restituisce la lista filtrata per operatore corrente
// voci con operatore vuoto → visibili a tutti
// voci con operatore valorizzato → solo se corrisponde a currentOperator_ o atShowAll_ è true
QStringList MainWindow::atFilteredList(const QString &key) const {
	// il filtro usa l'operatore attualmente selezionato nel campo, non l'username Windows
	const QString selectedOp = ui->comboBox_meta_operator->currentText();
	QStringList out;
	QSet<QString> seen;
	for (const auto &pair : atCsvData_.value(key))
		if (pair.second.isEmpty() || atShowAll_ || pair.second == selectedOp)
			if (!seen.contains(pair.first)) { seen.insert(pair.first); out << pair.first; }
	return out;
}

// [ADAPTRONICS] ripopola tutti i dropdown dipendenti dall'operatore
// chiamato all'avvio e ogni volta che cambia la spunta "Mostra tutto"
void MainWindow::repopulateAtDropdowns() {
	// Il campo Operator mostra SEMPRE tutti gli operatori, non viene filtrato.
	// blockSignals evita che il currentTextChanged dell'Operator scatti mentre lo ripopoliamo
	// e richiami repopulateAtDropdowns in loop.
	{
		QSignalBlocker blocker(ui->comboBox_meta_operator);
		QString prevOp = ui->comboBox_meta_operator->currentText();
		ui->comboBox_meta_operator->clear();
		for (const auto &pair : atCsvData_.value("OPERATOR"))
			ui->comboBox_meta_operator->addItem(pair.first);
		// ripristina l'operatore selezionato (o pre-seleziona l'username Windows se vuoto)
		const QString restoreOp = prevOp.isEmpty() ? currentOperator_ : prevOp;
		if (!restoreOp.isEmpty())
			ui->comboBox_meta_operator->setCurrentText(restoreOp);
		else
			ui->comboBox_meta_operator->setCurrentIndex(-1);
	}

	// helper: ripopola un QComboBox mantenendo il valore corrente se ancora valido
	auto repopPreserving = [](QComboBox *cb, const QStringList &items) {
		const QString prev = cb->currentText();
		cb->clear();
		cb->addItems(items);
		if (!prev.isEmpty() && items.contains(prev))
			cb->setCurrentText(prev);
		else
			cb->setCurrentIndex(-1);
	};

	// ID CAD: bloccato durante il repop per evitare che la cascata CAD→Patch scatti
	// con valori intermedi (clear, primo item, valore finale). La cascata viene
	// fatta una volta sola manualmente dopo, con il valore definitivo.
	{
		QSignalBlocker cadBlocker(ui->lineEdit_acq);
		repopPreserving(ui->lineEdit_acq, atFilteredList("CAD"));
	}

	// Material, Test: filtrati per operatore
	repopPreserving(ui->comboBox_meta_material, atFilteredList("MATERIAL"));
	repopPreserving(ui->comboBox_meta_test,     atFilteredList("TEST"));

	// ID Patch: cascata manuale dal CAD definitivo, preserva valore se ancora valido
	const QString currentCad = ui->lineEdit_acq->currentText();
	repopPreserving(ui->lineEdit_participant, atFilteredList("PATCH:" + currentCad));

	// Perno: liste globali, indipendenti da CAD e da operatore
	auto allItems = [this](const QString &key) {
		QStringList out;
		for (const auto &pair : atCsvData_.value(key))
			out << pair.first;
		return out;
	};
	repopPreserving(ui->comboBox_meta_perno_materiale, allItems("PERNO_MATERIALE"));
	repopPreserving(ui->comboBox_meta_perno_diametro,  allItems("PERNO_DIAMETRO"));
	repopPreserving(ui->comboBox_meta_perno_numero,    allItems("PERNO_NUMERO"));
	repopPreserving(ui->comboBox_meta_perno_posizione, allItems("PERNO_POSIZIONE"));
}

// [ADAPTRONICS] legge LR_Runtime_Entries.csv e popola atCsvData_ per le tendine a cascata
// formato CSV: TYPE,ID,PARENT_ID,OPERATOR (prima riga = intestazione, ignorata)
// OPERATOR opzionale: vuoto = voce visibile a tutti; valorizzato = visibile solo a quell'operatore
void MainWindow::loadAtCsv(const QString &cfgDir) {
	// [ADAPTRONICS] legge username Windows per pre-compilare il campo Operator e filtrare le voci
	currentOperator_ = QString::fromLocal8Bit(qgetenv("USERNAME"));

	QString csvPath = cfgDir + "/LR_Runtime_Entries.csv";
	QFile file(csvPath);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		qInfo() << "[ADAPTRONICS] LR_Runtime_Entries.csv non trovato in: " << cfgDir;
		return;
	}
	QTextStream in(&file);
	in.readLine(); // salta intestazione TYPE,ID,PARENT_ID,OPERATOR
	while (!in.atEnd()) {
		QString line = in.readLine().trimmed();
		if (line.isEmpty()) continue;
		QStringList parts = line.split(',');
		if (parts.size() < 2) continue;
		QString type       = parts[0].trimmed().toUpper();
		QString id         = parts[1].trimmed();
		QString parentId   = parts.size() > 2 ? parts[2].trimmed() : "";
		QString opFilter   = parts.size() > 3 ? parts[3].trimmed() : "";
		QString key        = parentId.isEmpty() ? type : type + ":" + parentId;
		atCsvData_[key].append(qMakePair(id, opFilter));
	}
	// [ADAPTRONICS] BEGIN — popola tutti i dropdown e collega i filtri "inizia con"

	// popola al primo caricamento (applica già il filtro operatore)
	repopulateAtDropdowns();

	// filtro "inizia con" per ID CAD
	connect(ui->lineEdit_acq->lineEdit(), &QLineEdit::textEdited, this, [this](const QString &text) {
		ui->lineEdit_acq->blockSignals(true);
		ui->lineEdit_acq->clear();
		for (const QString &item : atFilteredList("CAD"))
			if (text.isEmpty() || item.startsWith(text, Qt::CaseInsensitive))
				ui->lineEdit_acq->addItem(item);
		ui->lineEdit_acq->setEditText(text);
		ui->lineEdit_acq->blockSignals(false);
	});

	// filtro "inizia con" per ID Patch (figli del CAD corrente)
	connect(ui->lineEdit_participant->lineEdit(), &QLineEdit::textEdited, this, [this](const QString &text) {
		const QString cadText = ui->lineEdit_acq->currentText();
		ui->lineEdit_participant->blockSignals(true);
		ui->lineEdit_participant->clear();
		for (const QString &item : atFilteredList("PATCH:" + cadText))
			if (text.isEmpty() || item.startsWith(text, Qt::CaseInsensitive))
				ui->lineEdit_participant->addItem(item);
		ui->lineEdit_participant->setEditText(text);
		ui->lineEdit_participant->blockSignals(false);
	});

	// filtri per Operator, Material, Test
	auto connectSimpleFilter = [this](QComboBox *cb, const QString &key) {
		connect(cb->lineEdit(), &QLineEdit::textEdited, this, [this, cb, key](const QString &text) {
			cb->blockSignals(true);
			cb->clear();
			for (const QString &item : atFilteredList(key))
				if (text.isEmpty() || item.startsWith(text, Qt::CaseInsensitive))
					cb->addItem(item);
			cb->setEditText(text);
			cb->blockSignals(false);
		});
	};
	connectSimpleFilter(ui->comboBox_meta_material, "MATERIAL");
	connectSimpleFilter(ui->comboBox_meta_test,     "TEST");
	// Operator: filtro "inizia con" mostra sempre tutti (no filtro operatore su se stesso)
	connect(ui->comboBox_meta_operator->lineEdit(), &QLineEdit::textEdited, this, [this](const QString &text) {
		ui->comboBox_meta_operator->blockSignals(true);
		ui->comboBox_meta_operator->clear();
		for (const auto &pair : atCsvData_.value("OPERATOR"))
			if (text.isEmpty() || pair.first.startsWith(text, Qt::CaseInsensitive))
				ui->comboBox_meta_operator->addItem(pair.first);
		ui->comboBox_meta_operator->setEditText(text);
		ui->comboBox_meta_operator->blockSignals(false);
	});
	// quando l'operatore selezionato cambia → ripopola i dropdown filtrati
	connect(ui->comboBox_meta_operator, &QComboBox::currentTextChanged, this, [this](const QString &) {
		repopulateAtDropdowns();
	});

	// filtri per i 4 campi Perno: globali, indipendenti da CAD e operatore
	connectSimpleFilter(ui->comboBox_meta_perno_materiale, "PERNO_MATERIALE");
	connectSimpleFilter(ui->comboBox_meta_perno_diametro,  "PERNO_DIAMETRO");
	connectSimpleFilter(ui->comboBox_meta_perno_numero,    "PERNO_NUMERO");
	connectSimpleFilter(ui->comboBox_meta_perno_posizione, "PERNO_POSIZIONE");

	// spunta "Mostra tutto": toglie il filtro operatore e ripopola tutte le tendine
	connect(ui->checkBox_showAll, &QCheckBox::toggled, this, [this](bool checked) {
		atShowAll_ = checked;
		repopulateAtDropdowns();
	});
	// [ADAPTRONICS] END — filtri
}
// [FINE ADAPTRONICS]

MainWindow::~MainWindow() noexcept = default;

void MainWindow::rcsCheckBoxChanged(bool checked) { enableRcs(checked); }

void MainWindow::enableRcs(bool bEnable) {
	if (rcs) {
		if (!bEnable) {
			disconnect(rcs.get());
            rcs = nullptr;
        }
	} else if (bEnable) {
		uint16_t port = ui->rcsport->value();
		rcs = std::make_unique<RemoteControlSocket>(port);
		// TODO: Add some method to RemoteControlSocket to report if its server is listening (i.e. was successful).
		connect(rcs.get(), &RemoteControlSocket::refresh_streams, this, &MainWindow::refreshStreams);
		connect(rcs.get(), &RemoteControlSocket::start, this, &MainWindow::rcsStartRecording);
		connect(rcs.get(), &RemoteControlSocket::stop, this, &MainWindow::rcsStopRecording);
		connect(rcs.get(), &RemoteControlSocket::filename, this, &MainWindow::rcsUpdateFilename);
		connect(rcs.get(), &RemoteControlSocket::select_all, this, &MainWindow::selectAllStreams);
		connect(rcs.get(), &RemoteControlSocket::select_none, this, &MainWindow::selectNoStreams);
	}
	bool oldState = ui->rcsCheckBox->blockSignals(true);
	ui->rcsCheckBox->setChecked(bEnable);
	ui->rcsCheckBox->blockSignals(oldState);
}

void MainWindow::rcsportValueChangedInt(int value) {
	if (rcs) {
        enableRcs(false);  // Will also uncheck box.
		enableRcs(true);   // Will also check box.
    }
}

void MainWindow::rcsStartRecording() {
	// since we want to avoid a pop-up window when streams are missing or unchecked,
	// we'll check all the streams and start recording
	hideWarnings = true;
	selectAllStreams();
	startRecording();
}

void MainWindow::rcsStopRecording() {
	hideWarnings = true;
	stopRecording();
}

void MainWindow::rcsUpdateFilename(QString s) {
	//
	// format: "filename {option:value}{option:value}
	// Options are:
	//	root: full path to Study root;
	//  template: legacy filename template, left to default (bids) if unspecified;
	//	task; run; participant; session; acquisition: base options
	//	(BIDS) modality: from either the defaults eeg, ieeg, meg, beh or adding a new
	//		potentially unsupported value.
	QRegularExpression re("{(?P<option>\\w+?):(?P<value>[^}]*)}");

	QRegularExpressionMatchIterator i = re.globalMatch(s);
	while (i.hasNext()) {
		QRegularExpressionMatch match = i.next();
		QString option = match.captured("option");
		QString value = match.captured("value");
		// TODO: replace with QStringList and switch
		if (option.toLower() == "root") {
			ui->rootEdit->setText(QDir::toNativeSeparators(value));
		} else if (option.toLower() == "template") {
			// legacy
			ui->check_bids->setChecked(false);
			ui->lineEdit_template->setText(value.toLower());
		} else if (option.toLower() == "task") {
			ui->input_blocktask->clear();
			ui->input_blocktask->addItem(value);
			ui->input_blocktask->setCurrentIndex(0);
		} else if (option.toLower() == "run") {
			ui->spin_counter->setValue(value.toInt());
		} else if (option.toLower() == "participant") {
			ui->lineEdit_participant->setCurrentText(value); // [ADAPTRONICS] setText → setCurrentText per QComboBox
		} else if (option.toLower() == "session") {
			ui->lineEdit_session->setCurrentText(value); // [ADAPTRONICS] setText → setCurrentText per QComboBox
		} else if (option.toLower() == "acquisition") {
			ui->lineEdit_acq->setCurrentText(value); // [ADAPTRONICS] setText → setCurrentText per QComboBox
		} else if (option.toLower() == "modality") {
			if (ui->input_modality->findText(value.toLower()) != -1)
				ui->input_modality->setCurrentIndex(ui->input_modality->findText(value.toLower()));
			else {
				ui->input_modality->insertItem(ui->input_modality->count(), value.toLower());
				ui->input_modality->setCurrentIndex(ui->input_modality->count() - 1);
			}
		}
	}
	// to make sure all the values are updated.
	printReplacedFilename();
}
