/*
    EDIScope - DAB+/EDI diagnostic player
    Copyright (C) 2025-2026 Yoann Queret

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ediscope_qt_mnsc.h"
#include "ediscope_qt_theme.h"

#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTextStream>
#include <QVBoxLayout>


// the list of changes keeps the most recent ones only
static const size_t CHANGES_MAX = 5000;

static QTableWidget* CreateTable(const QStringList& headers, QWidget *parent) {
	QTableWidget *table = new QTableWidget(0, headers.size(), parent);
	table->setHorizontalHeaderLabels(headers);
	table->verticalHeader()->hide();
	table->verticalHeader()->setDefaultSectionSize(22);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setShowGrid(false);
	table->setAlternatingRowColors(true);
	table->setWordWrap(false);
	table->horizontalHeader()->setHighlightSections(false);
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	table->horizontalHeader()->setStretchLastSection(true);
	EDIScopeQtTheme::MakeTableCopyable(table);
	return table;
}

static QTableWidgetItem* SetCell(QTableWidget *table, int row, int column, const QString& text, bool mono = false) {
	QTableWidgetItem *item = new QTableWidgetItem(text);
	if(mono)
		item->setFont(EDIScopeQtTheme::MonoFont());
	table->setItem(row, column, item);
	return item;
}

// a number, sorted as such
static void SetNumberCell(QTableWidget *table, int row, int column, qlonglong value) {
	QTableWidgetItem *item = new QTableWidgetItem();
	item->setData(Qt::DisplayRole, value);
	item->setFont(EDIScopeQtTheme::MonoFont());
	item->setTextAlignment(Qt::AlignCenter);
	table->setItem(row, column, item);
}

static QString Time(const QDateTime& time) {
	return time.isValid() ? time.toString("HH:mm:ss") : QString();
}


// --- EDIScopeQtMNSCWindow -----------------------------------------------------------------
EDIScopeQtMNSCWindow::EDIScopeQtMNSCWindow(QWidget *parent) :
		QWidget(nullptr, Qt::Window), changes_serial(1), changes_shown_serial(0), messages(0), crc_errors(0), dirty(true), positioned(false) {
	main_window = parent;
	setWindowTitle("MNSC");
	resize(1080, 680);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);

	// --- summary + actions ---
	QHBoxLayout *header = new QHBoxLayout();
	summary_label = new QLabel(this);
	summary_label->setFont(EDIScopeQtTheme::MonoFont());
	summary_label->setTextFormat(Qt::RichText);
	header->addWidget(summary_label, 1);

	QPushButton *export_button = new QPushButton("Export...", this);
	export_button->setToolTip("Save the transmitters as a CSV file");
	connect(export_button, &QPushButton::clicked, this, &EDIScopeQtMNSCWindow::Export);
	header->addWidget(export_button);

	QPushButton *reset_button = new QPushButton("Reset", this);
	reset_button->setToolTip("Clear the transmitters and the changes - they start again from now");
	connect(reset_button, &QPushButton::clicked, this, &EDIScopeQtMNSCWindow::Clear);
	header->addWidget(reset_button);
	layout->addLayout(header);

	QLabel *note = new QLabel("Transmitter Set-up and Control information (ETS 300 799 annex A.3.2.1, ASS message type 0) "
			"carried in the Multiplex Network Signalling Channel of the ETI: the offset delay and TII code the multiplexer "
			"signals to each transmitter of the network. Times are local, as received.", this);
	note->setWordWrap(true);
	EDIScopeQtTheme::SetRole(note, "muted");
	layout->addWidget(note);

	QSplitter *splitter = new QSplitter(Qt::Vertical, this);
	splitter->setChildrenCollapsible(false);
	layout->addWidget(splitter, 1);

	QWidget *transmitters_box = new QWidget(splitter);
	QVBoxLayout *transmitters_layout = new QVBoxLayout(transmitters_box);
	transmitters_layout->setContentsMargins(0, 0, 0, 0);
	QLabel *transmitters_title = new QLabel("Transmitters", transmitters_box);
	EDIScopeQtTheme::SetRole(transmitters_title, "subtitle");
	transmitters_layout->addWidget(transmitters_title);
	transmitters_view = CreateTable({"Address", "TII MainId", "TII SubId", "Offset delay (µs)", "Control flags",
			"Messages", "Last interval", "First seen", "Last seen"}, transmitters_box);
	transmitters_view->setSortingEnabled(true);
	transmitters_view->sortByColumn(0, Qt::AscendingOrder);
	transmitters_layout->addWidget(transmitters_view, 1);
	splitter->addWidget(transmitters_box);

	QWidget *changes_box = new QWidget(splitter);
	QVBoxLayout *changes_layout = new QVBoxLayout(changes_box);
	changes_layout->setContentsMargins(0, 6, 0, 0);
	QLabel *changes_title = new QLabel("Changes of the values signalled for a transmitter", changes_box);
	EDIScopeQtTheme::SetRole(changes_title, "subtitle");
	changes_layout->addWidget(changes_title);
	changes_view = CreateTable({"Time", "Address", "Value", "Before", "After"}, changes_box);
	changes_layout->addWidget(changes_view, 1);
	splitter->addWidget(changes_box);
	splitter->setSizes({420, 160});

	// messages arrive a few times per second: the tables are refreshed once per second at most
	refresh_timer.setInterval(1000);
	connect(&refresh_timer, &QTimer::timeout, this, [this]() {
		if(dirty)
			Refresh();
	});

	Refresh();
}

void EDIScopeQtMNSCWindow::closeEvent(QCloseEvent *event) {
	// the application is quitting (the main window is gone): let it close for real
	if(!main_window || !main_window->isVisible()) {
		event->accept();
		return;
	}

	// just hidden; the main window un-toggles its button
	hide();
	event->ignore();
	emit Closed();
}

void EDIScopeQtMNSCWindow::showEvent(QShowEvent *event) {
	Refresh();
	refresh_timer.start();
	QWidget::showEvent(event);
}

void EDIScopeQtMNSCWindow::hideEvent(QHideEvent *event) {
	refresh_timer.stop();
	QWidget::hideEvent(event);
}

void EDIScopeQtMNSCWindow::AlignToParent() {
	if(positioned || !main_window)
		return;
	positioned = true;

	EDIScopeQtTheme::CenterOnWindow(this, main_window);
}

QString EDIScopeQtMNSCWindow::AddressString(uint16_t address) {
	return QString::asprintf("0x%04X", address);
}

QString EDIScopeQtMNSCWindow::OffsetString(int offset_delay_us) {
	return QString::number(offset_delay_us) + " µs";
}

QString EDIScopeQtMNSCWindow::FlagsString(uint16_t flags) {
	return QString::asprintf("0x%04X", flags);
}


// --- state -----------------------------------------------------------------
void EDIScopeQtMNSCWindow::AddTSC(const MNSC_TSC& tsc) {
	QDateTime now = QDateTime::currentDateTime();
	messages++;

	std::map<uint16_t, ENTRY>::iterator it = transmitters.find(tsc.address);
	if(it == transmitters.end()) {
		ENTRY entry;
		entry.tsc = tsc;
		entry.messages = 1;
		entry.first_seen = now;
		entry.last_seen = now;
		entry.last_interval_ms = -1;
		transmitters.emplace(tsc.address, entry);
	} else {
		ENTRY& entry = it->second;
		auto change = [&](const QString& field, const QString& old_value, const QString& new_value) {
			if(old_value == new_value)
				return;
			CHANGE c;
			c.time = now;
			c.address = tsc.address;
			c.field = field;
			c.old_value = old_value;
			c.new_value = new_value;
			changes.push_back(c);
			changes_serial++;
		};
		change("TII", QString("%1 / %2").arg(entry.tsc.tii_main_id).arg(entry.tsc.tii_sub_id), QString("%1 / %2").arg(tsc.tii_main_id).arg(tsc.tii_sub_id));
		change("Offset delay", OffsetString(entry.tsc.offset_delay_us), OffsetString(tsc.offset_delay_us));
		change("Control flags", FlagsString(entry.tsc.control_flags), FlagsString(tsc.control_flags));
		if(changes.size() > CHANGES_MAX)
			changes.erase(changes.begin(), changes.begin() + (changes.size() - CHANGES_MAX));

		entry.tsc = tsc;
		entry.messages++;
		entry.last_interval_ms = entry.last_seen.msecsTo(now);
		entry.last_seen = now;
	}
	dirty = true;
}

void EDIScopeQtMNSCWindow::AddCRCError() {
	crc_errors++;
	dirty = true;
}

void EDIScopeQtMNSCWindow::Clear() {
	transmitters.clear();
	changes.clear();
	changes_serial++;
	messages = 0;
	crc_errors = 0;
	dirty = true;
	Refresh();
}


// --- display -----------------------------------------------------------------
void EDIScopeQtMNSCWindow::Refresh() {
	dirty = false;

	// --- summary ---
	if(!messages && !crc_errors) {
		summary_label->setText("No transmitter information (TSC) received in the MNSC.");
	} else {
		QString crc = QString::number(crc_errors);
		if(crc_errors)
			crc = "<span style=\"color: " + EDIScopeQtTheme::error.name() + ";\"><b>✗ " + crc + "</b></span>";
		summary_label->setText(QString("Transmitters: <b>%1</b> &nbsp;·&nbsp; Messages: %2 &nbsp;·&nbsp; CRC errors: %3 &nbsp;·&nbsp; Changes: %4")
				.arg(transmitters.size()).arg(messages).arg(crc).arg(changes.size()));
	}

	// --- transmitters (keeping the sort order and the selected address) ---
	QString selected;
	QList<QTableWidgetItem*> selected_items = transmitters_view->selectedItems();
	if(!selected_items.isEmpty())
		selected = transmitters_view->item(selected_items.first()->row(), 0)->text();

	transmitters_view->setSortingEnabled(false);
	transmitters_view->setRowCount((int) transmitters.size());
	int row = 0;
	for(const std::pair<const uint16_t, ENTRY>& kv : transmitters) {
		const ENTRY& e = kv.second;
		SetCell(transmitters_view, row, 0, AddressString(kv.first), true);
		SetNumberCell(transmitters_view, row, 1, e.tsc.tii_main_id);
		SetNumberCell(transmitters_view, row, 2, e.tsc.tii_sub_id);
		SetNumberCell(transmitters_view, row, 3, e.tsc.offset_delay_us);
		SetCell(transmitters_view, row, 4, FlagsString(e.tsc.control_flags), true)->setTextAlignment(Qt::AlignCenter);
		SetNumberCell(transmitters_view, row, 5, (qlonglong) e.messages);
		SetCell(transmitters_view, row, 6, e.last_interval_ms >= 0 ? QString::number(e.last_interval_ms / 1000.0, 'f', 1) + " s" : QString(), true)
				->setTextAlignment(Qt::AlignCenter);
		SetCell(transmitters_view, row, 7, Time(e.first_seen), true);
		SetCell(transmitters_view, row, 8, Time(e.last_seen), true);
		row++;
	}
	transmitters_view->setSortingEnabled(true);
	transmitters_view->clearSelection();
	for(int r = 0; r < transmitters_view->rowCount(); r++)
		if(transmitters_view->item(r, 0)->text() == selected)
			transmitters_view->selectRow(r);
	if(transmitters_view->rowCount() && !transmitters_view->property("sized").toBool()) {
		transmitters_view->resizeColumnsToContents();
		transmitters_view->setProperty("sized", true);
	}

	// --- changes, the most recent first ---
	if(changes_shown_serial != changes_serial) {
		changes_shown_serial = changes_serial;
		changes_view->setRowCount((int) changes.size());
		for(size_t i = 0; i < changes.size(); i++) {
			const CHANGE& c = changes[changes.size() - 1 - i];
			int r = (int) i;
			SetCell(changes_view, r, 0, Time(c.time), true);
			SetCell(changes_view, r, 1, AddressString(c.address), true);
			SetCell(changes_view, r, 2, c.field);
			SetCell(changes_view, r, 3, c.old_value, true);
			SetCell(changes_view, r, 4, c.new_value, true);
		}
		if(changes_view->rowCount() && !changes_view->property("sized").toBool()) {
			changes_view->resizeColumnsToContents();
			changes_view->setProperty("sized", true);
		}
	}
}

void EDIScopeQtMNSCWindow::Export() {
	QString name = "mnsc-transmitters-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".csv";
	QString filename = QFileDialog::getSaveFileName(this, "Export Transmitters", QDir::home().filePath(name), "CSV files (*.csv);;All files (*)");
	if(filename.isEmpty())
		return;

	QFile file(filename);
	if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		QMessageBox::critical(this, "Export Transmitters", "Cannot write \"" + filename + "\":\n" + file.errorString());
		return;
	}

	QTextStream out(&file);
	out << "Address,TII MainId,TII SubId,Offset delay (us),Control flags,Messages,First seen,Last seen\n";
	for(const std::pair<const uint16_t, ENTRY>& kv : transmitters) {
		const ENTRY& e = kv.second;
		out << AddressString(kv.first) << "," << e.tsc.tii_main_id << "," << e.tsc.tii_sub_id << "," << e.tsc.offset_delay_us << ","
				<< FlagsString(e.tsc.control_flags) << "," << e.messages << ","
				<< e.first_seen.toString("yyyy-MM-dd HH:mm:ss") << "," << e.last_seen.toString("yyyy-MM-dd HH:mm:ss") << "\n";
	}
}
