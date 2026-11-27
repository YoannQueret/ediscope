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

#include "ediscope_qt_announcements.h"
#include "ediscope_qt_theme.h"

#include <algorithm>

#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTextStream>
#include <QVBoxLayout>

#include "tools.h"


// an announcement still active after this long is reported (for information)
static const qint64 LONG_ANNOUNCEMENT_MS = 10 * 60 * 1000;

// the history keeps the most recent events only
static const size_t HISTORY_MAX_EVENTS = 5000;

static const uint8_t ALARM_CLUSTER = 0xFF;
static const uint16_t ALARM_FLAG = 0x0001;

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

// a coloured cell, also bold - always together with a distinct symbol in its text, never by colour alone
static void SetColoredCell(QTableWidget *table, int row, int column, const QString& text, const QColor& color, bool bold) {
	QTableWidgetItem *item = SetCell(table, row, column, text);
	item->setForeground(color);
	if(bold) {
		QFont font = item->font();
		font.setBold(true);
		item->setFont(font);
	}
}

// sizes the columns to their content once, the first time the table gets rows
static void SizeOnce(QTableWidget *table) {
	if(table->rowCount() && !table->property("sized").toBool()) {
		table->resizeColumnsToContents();
		table->setProperty("sized", true);
	}
}

static QString Duration(qint64 ms) {
	if(ms < 0)
		return QString();
	qint64 s = ms / 1000;
	if(s < 60)
		return QString("%1 s").arg(s);
	if(s < 3600)
		return QString("%1 min %2 s").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
	return QString("%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60, 2, 10, QChar('0'));
}

static QString Hex(int value, int nibbles) {
	return QString::fromStdString(StringTools::IntToHex(value, nibbles));
}

static QString Time(const QDateTime& time) {
	return time.isValid() ? time.toString("HH:mm:ss") : QString();
}


// --- EDIScopeQtAnnouncementsWindow -----------------------------------------------------------------
EDIScopeQtAnnouncementsWindow::EDIScopeQtAnnouncementsWindow(QWidget *parent) :
		QWidget(nullptr, Qt::Window), ensemble_known(false), history_serial(1), history_shown_serial(0), positioned(false) {
	main_window = parent;
	setWindowTitle("Announcements");
	resize(1100, 680);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);

	// --- summary + actions ---
	QHBoxLayout *header = new QHBoxLayout();
	summary_label = new QLabel(this);
	summary_label->setFont(EDIScopeQtTheme::MonoFont());
	summary_label->setTextFormat(Qt::RichText);
	header->addWidget(summary_label, 1);

	QPushButton *fig_button = new QPushButton("FIG Analysis...", this);
	fig_button->setToolTip("Repetition rates of FIG 0/18, 0/19, 0/25 and 0/26 against ETSI TS 103 176");
	connect(fig_button, &QPushButton::clicked, this, &EDIScopeQtAnnouncementsWindow::FIGAnalysisRequested);
	header->addWidget(fig_button);

	QPushButton *export_button = new QPushButton("Export History...", this);
	export_button->setToolTip("Save the history as a CSV file");
	connect(export_button, &QPushButton::clicked, this, &EDIScopeQtAnnouncementsWindow::ExportHistory);
	header->addWidget(export_button);

	QPushButton *reset_button = new QPushButton("Reset", this);
	reset_button->setToolTip("Clear the history and the checks - they start again from now");
	connect(reset_button, &QPushButton::clicked, this, [this]() {
		history.clear();
		history_serial++;
		findings.clear();
		Evaluate();
		Refresh();
	});
	header->addWidget(reset_button);
	layout->addLayout(header);

	QLabel *note = new QLabel("ETSI EN 300 401 V2.1.1 clause 8.1.6. States: ▶ announcement in progress, ■ ended, "
			"✗ lost (no longer signalled, without its end). Times are local, as received.", this);
	note->setWordWrap(true);
	EDIScopeQtTheme::SetRole(note, "muted");
	layout->addWidget(note);

	tabs = new QTabWidget(this);
	layout->addWidget(tabs, 1);

	// --- "Clusters" tab ---
	QSplitter *clusters_splitter = new QSplitter(Qt::Vertical, tabs);
	clusters_splitter->setChildrenCollapsible(false);
	QWidget *clusters_box = new QWidget(clusters_splitter);
	QVBoxLayout *clusters_layout = new QVBoxLayout(clusters_box);
	clusters_layout->setContentsMargins(0, 6, 0, 0);
	QLabel *clusters_title = new QLabel("This ensemble (FIG 0/19)", clusters_box);
	EDIScopeQtTheme::SetRole(clusters_title, "subtitle");
	clusters_layout->addWidget(clusters_title);
	clusters_view = CreateTable({"Cluster", "State", "Types", "SubChId", "Carried by", "New", "Since", "Duration", "Services (FIG 0/18)"}, clusters_box);
	clusters_layout->addWidget(clusters_view, 1);
	clusters_splitter->addWidget(clusters_box);

	QWidget *oe_clusters_box = new QWidget(clusters_splitter);
	QVBoxLayout *oe_clusters_layout = new QVBoxLayout(oe_clusters_box);
	oe_clusters_layout->setContentsMargins(0, 6, 0, 0);
	QLabel *oe_clusters_title = new QLabel("Other ensembles (FIG 0/26)", oe_clusters_box);
	EDIScopeQtTheme::SetRole(oe_clusters_title, "subtitle");
	oe_clusters_layout->addWidget(oe_clusters_title);
	oe_clusters_view = CreateTable({"OE cluster", "State", "Types", "Target", "SCIdS", "New", "Since", "Duration", "Services (FIG 0/25)"}, oe_clusters_box);
	oe_clusters_layout->addWidget(oe_clusters_view, 1);
	clusters_splitter->addWidget(oe_clusters_box);
	clusters_splitter->setSizes({330, 200});
	tabs->addTab(clusters_splitter, "Clusters");

	// --- "Support" tab ---
	QSplitter *support_splitter = new QSplitter(Qt::Vertical, tabs);
	support_splitter->setChildrenCollapsible(false);
	QWidget *support_box = new QWidget(support_splitter);
	QVBoxLayout *support_layout = new QVBoxLayout(support_box);
	support_layout->setContentsMargins(0, 6, 0, 0);
	QLabel *support_title = new QLabel("Announcements of this ensemble that may interrupt a service (FIG 0/18)", support_box);
	EDIScopeQtTheme::SetRole(support_title, "subtitle");
	support_layout->addWidget(support_title);
	support_view = CreateTable({"SId", "Service", "Supported types", "Clusters"}, support_box);
	support_layout->addWidget(support_view, 1);
	support_splitter->addWidget(support_box);

	QWidget *oe_support_box = new QWidget(support_splitter);
	QVBoxLayout *oe_support_layout = new QVBoxLayout(oe_support_box);
	oe_support_layout->setContentsMargins(0, 6, 0, 0);
	QLabel *oe_support_title = new QLabel("Announcements of other ensembles that may interrupt a service (FIG 0/25)", oe_support_box);
	EDIScopeQtTheme::SetRole(oe_support_title, "subtitle");
	oe_support_layout->addWidget(oe_support_title);
	oe_support_view = CreateTable({"SId", "Service", "Supported types", "Targets (OE cluster @ EId)"}, oe_support_box);
	oe_support_layout->addWidget(oe_support_view, 1);
	support_splitter->addWidget(oe_support_box);
	support_splitter->setSizes({330, 200});
	tabs->addTab(support_splitter, "Support");

	// --- "History" tab ---
	history_view = CreateTable({"Time", "FIG", "Cluster", "Event", "Types", "SubChId / Target", "Carried by", "Duration", "Note"}, tabs);
	tabs->addTab(history_view, "History");

	// --- "Checks" tab ---
	checks_view = CreateTable({"Severity", "FIG", "Entity", "Finding", "First seen", "Last seen", "State"}, tabs);
	tabs->addTab(checks_view, "Checks");

	// durations, "long announcement" check
	tick_timer.setInterval(1000);
	connect(&tick_timer, &QTimer::timeout, this, [this]() {
		Evaluate();
		if(isVisible())
			Refresh();
	});
	tick_timer.start();

	Refresh();
}

void EDIScopeQtAnnouncementsWindow::closeEvent(QCloseEvent *event) {
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

void EDIScopeQtAnnouncementsWindow::showEvent(QShowEvent *event) {
	Refresh();
	QWidget::showEvent(event);
}

void EDIScopeQtAnnouncementsWindow::AlignToParent() {
	if(positioned || !main_window)
		return;
	positioned = true;

	EDIScopeQtTheme::CenterOnWindow(this, main_window);
}

QString EDIScopeQtAnnouncementsWindow::TypesString(uint16_t flags) {
	QString result;
	for(int type = 0; type < 16; type++) {
		if(!(flags & (1 << type)))
			continue;
		if(!result.isEmpty())
			result += " + ";
		result += QString::fromStdString(FICDecoder::ConvertASuTypeToString(type));
	}
	return result;
}

QString EDIScopeQtAnnouncementsWindow::SeverityText(Severity severity) {
	// a symbol as well as a colour, so the severities never rely on colour alone
	switch(severity) {
	case Severity::ERROR:	return "✗ Error";
	case Severity::WARNING:	return "▲ Warning";
	default:				return "ℹ Info";
	}
}

QColor EDIScopeQtAnnouncementsWindow::SeverityColor(Severity severity) {
	switch(severity) {
	case Severity::ERROR:	return EDIScopeQtTheme::error;
	case Severity::WARNING:	return EDIScopeQtTheme::warning;
	default:				return EDIScopeQtTheme::info;
	}
}

QString EDIScopeQtAnnouncementsWindow::ClusterName(uint8_t cid) {
	return Hex(cid, 2) + (cid == ALARM_CLUSTER ? " (alarm)" : "");
}

QString EDIScopeQtAnnouncementsWindow::SubchannelService(int subchid) const {
	QString result;
	for(const SERVICE_ENTRY& s : services) {
		if(s.subchid != subchid)
			continue;
		if(!result.isEmpty())
			result += ", ";
		result += QString::fromStdString(s.label);
	}
	return result;
}

QString EDIScopeQtAnnouncementsWindow::ServiceName(int sid) const {
	for(const SERVICE_ENTRY& s : services)
		if(s.sid == sid && s.primary)
			return QString::fromStdString(s.label);
	return QString();
}

std::vector<const EDIScopeQtAnnouncementsWindow::SERVICE_ENTRY*> EDIScopeQtAnnouncementsWindow::ClusterMembers(uint8_t cid) const {
	std::vector<const SERVICE_ENTRY*> members;
	for(const SERVICE_ENTRY& s : services)
		if(s.primary && s.cids.count(cid))
			members.push_back(&s);
	return members;
}


// --- state updates -----------------------------------------------------------------
void EDIScopeQtAnnouncementsWindow::AddEvent(const QString& fig, const QString& cluster, const QString& event, uint16_t asw_flags,
		const QString& where, const QString& carried_by, qint64 duration_ms, const QString& note) {
	EVENT e;
	e.time = QDateTime::currentDateTime();
	e.fig = fig;
	e.cluster = cluster;
	e.event = event;
	e.types = TypesString(asw_flags);
	e.where = where;
	e.carried_by = carried_by;
	e.duration_ms = duration_ms;
	e.note = note;
	history.push_back(e);
	history_serial++;
	if(history.size() > HISTORY_MAX_EVENTS)
		history.erase(history.begin(), history.begin() + (history.size() - HISTORY_MAX_EVENTS));
}

void EDIScopeQtAnnouncementsWindow::SetEnsemble(const FIC_ENSEMBLE& new_ensemble) {
	QDateTime now = QDateTime::currentDateTime();

	// compare every cluster, known before or now: start, change, end or loss of an announcement
	std::set<uint8_t> cids;
	for(const asw_clusters_t::value_type& kv : ensemble.asw_clusters)
		cids.insert(kv.first);
	for(const asw_clusters_t::value_type& kv : new_ensemble.asw_clusters)
		cids.insert(kv.first);

	for(uint8_t cid : cids) {
		asw_clusters_t::const_iterator old_it = ensemble.asw_clusters.find(cid);
		asw_clusters_t::const_iterator new_it = new_ensemble.asw_clusters.find(cid);
		bool known_before = old_it != ensemble.asw_clusters.cend();
		FIC_ASW_CLUSTER old_ac = known_before ? old_it->second : FIC_ASW_CLUSTER();
		FIC_ASW_CLUSTER new_ac = new_it != new_ensemble.asw_clusters.cend() ? new_it->second : FIC_ASW_CLUSTER();

		QString where = new_ac.IsNone() ? QString() : QString::number(new_ac.subchid);
		QString carried_by = new_ac.IsNone() ? QString() : SubchannelService(new_ac.subchid);

		if(!old_ac.IsActive() && new_ac.IsActive()) {
			QString note;
			if(old_ac.lost)
				note = "signalled again after being lost";
			else if(!known_before && !new_ac.new_flag)
				note = "already in progress when first received";
			else if(!new_ac.new_flag)
				note = "New flag not set";
			since[cid] = now;
			AddEvent("0/19", ClusterName(cid), "▶ Start", new_ac.asw_flags, where, carried_by, -1, note);
		} else if(old_ac.IsActive() && new_ac.IsActive()) {
			if(old_ac.asw_flags != new_ac.asw_flags || old_ac.subchid != new_ac.subchid) {
				QString note;
				if(old_ac.asw_flags != new_ac.asw_flags)
					note = "was " + TypesString(old_ac.asw_flags);
				if(old_ac.subchid != new_ac.subchid)
					note += QString(note.isEmpty() ? "" : ", ") + "was SubChId " + QString::number(old_ac.subchid);
				AddEvent("0/19", ClusterName(cid), "◆ Change", new_ac.asw_flags, where, carried_by, -1, note);
			}
		} else if(old_ac.IsActive() && !new_ac.IsActive()) {
			qint64 duration = since.count(cid) ? since[cid].msecsTo(now) : -1;
			since.erase(cid);
			if(new_ac.lost)
				AddEvent("0/19", ClusterName(cid), "✗ Lost", old_ac.asw_flags, where, carried_by, duration,
						"no longer signalled, without its end (ASw flags all zero)");
			else
				AddEvent("0/19", ClusterName(cid), "■ End", old_ac.asw_flags, where, carried_by, duration, QString());
		}
	}

	if(ensemble.al_flag != new_ensemble.al_flag && ensemble_known)
		AddEvent("0/0", "-", new_ensemble.al_flag ? "Al flag set" : "Al flag cleared", 0, QString(), QString(), -1,
				new_ensemble.al_flag ? "alarm announcements enabled" : "alarm announcements disabled");

	ensemble = new_ensemble;
	ensemble_known = !ensemble.IsNone();
	Evaluate();
	if(isVisible())
		Refresh();
}

void EDIScopeQtAnnouncementsWindow::SetServices(const std::vector<SERVICE_ENTRY>& services) {
	this->services = services;
	Evaluate();
	if(isVisible())
		Refresh();
}

void EDIScopeQtAnnouncementsWindow::SetSubchannels(const std::set<int>& subchids) {
	this->subchids = subchids;
	Evaluate();
}

void EDIScopeQtAnnouncementsWindow::UpdateOESupport(const FIC_OE_ANNOUNCEMENT_SUPPORT& support) {
	oe_supports[support.sid] = support;
	Evaluate();
	if(isVisible())
		Refresh();
}

void EDIScopeQtAnnouncementsWindow::UpdateOESwitching(uint32_t key, const FIC_OE_ASW_CLUSTER& cluster) {
	QDateTime now = QDateTime::currentDateTime();

	fic_oe_asw_clusters_t::const_iterator old_it = oe_clusters.find(key);
	bool known_before = old_it != oe_clusters.cend();
	FIC_OE_ASW_CLUSTER old_ac = known_before ? old_it->second : FIC_OE_ASW_CLUSTER();
	const FIC_OE_ASW_CLUSTER& new_ac = cluster;

	bool alarm = new_ac.oe_cid == ALARM_CLUSTER;
	QString where = alarm ? "EId " + Hex(new_ac.target_sid, 4).mid(2) : "SId " + Hex(new_ac.target_sid, 4).mid(2) + "/" + QString::number(new_ac.scids);

	if(!old_ac.IsActive() && new_ac.IsActive()) {
		QString note;
		if(old_ac.lost)
			note = "signalled again after being lost";
		else if(!known_before && !new_ac.new_flag)
			note = "already in progress when first received";
		else if(!new_ac.new_flag)
			note = "New flag not set";
		oe_since[key] = now;
		AddEvent("0/26", ClusterName(new_ac.oe_cid), "▶ Start", new_ac.asw_flags, where, QString(), -1, note);
	} else if(old_ac.IsActive() && new_ac.IsActive()) {
		if(old_ac.asw_flags != new_ac.asw_flags || old_ac.scids != new_ac.scids)
			AddEvent("0/26", ClusterName(new_ac.oe_cid), "◆ Change", new_ac.asw_flags, where, QString(), -1,
					old_ac.asw_flags != new_ac.asw_flags ? "was " + TypesString(old_ac.asw_flags) : "was SCIdS " + QString::number(old_ac.scids));
	} else if(old_ac.IsActive() && !new_ac.IsActive()) {
		qint64 duration = oe_since.count(key) ? oe_since[key].msecsTo(now) : -1;
		oe_since.erase(key);
		if(new_ac.lost)
			AddEvent("0/26", ClusterName(new_ac.oe_cid), "✗ Lost", old_ac.asw_flags, where, QString(), duration,
					"no longer signalled, without its end (ASw flags all zero)");
		else
			AddEvent("0/26", ClusterName(new_ac.oe_cid), "■ End", old_ac.asw_flags, where, QString(), duration, QString());
	}

	oe_clusters[key] = cluster;
	Evaluate();
	if(isVisible())
		Refresh();
}

void EDIScopeQtAnnouncementsWindow::Clear() {
	ensemble = FIC_ENSEMBLE();
	ensemble_known = false;
	services.clear();
	subchids.clear();
	oe_supports.clear();
	oe_clusters.clear();
	since.clear();
	oe_since.clear();
	history.clear();
	history_serial++;
	findings.clear();
	Refresh();
}


// --- checks -----------------------------------------------------------------
void EDIScopeQtAnnouncementsWindow::Evaluate() {
	if(!ensemble_known)
		return;

	QDateTime now = QDateTime::currentDateTime();
	std::set<QString> present;
	auto add = [&](Severity severity, const QString& fig, const QString& entity, const QString& text) {
		QString key = fig + "|" + entity + "|" + text;
		present.insert(key);
		std::map<QString, FINDING>::iterator it = findings.find(key);
		if(it == findings.end()) {
			FINDING f;
			f.severity = severity;
			f.fig = fig;
			f.entity = entity;
			f.text = text;
			f.first_seen = now;
			it = findings.emplace(key, f).first;
		}
		it->second.last_seen = now;
		it->second.present = true;
	};

	// --- FIG 0/18 ---
	for(const SERVICE_ENTRY& s : services) {
		if(!s.primary)
			continue;
		QString entity = "SId " + QString::fromStdString(StringTools::SidToHex(s.sid)) + " " + QString::fromStdString(s.label);
		if(s.asu_flags & ALARM_FLAG)
			add(Severity::ERROR, "0/18", entity, "ASu flag b0 (Alarm) set - it shall be 0: alarm announcements are enabled ensemble-wide by the Al flag of FIG 0/0");
		for(uint8_t cid : s.cids)
			if(cid == 0x00 || cid == ALARM_CLUSTER)
				add(Severity::ERROR, "0/18", entity, "Cluster Id " + Hex(cid, 2) + " signalled - Cluster Ids 0x00 and 0xFF are pre-defined and shall not be signalled in FIG 0/18");
		if(s.asu_flags && s.cids.empty())
			add(Severity::WARNING, "0/18", entity, "Announcement types supported, but in no cluster - the service can never be interrupted");
		if(!s.asu_flags && !s.cids.empty())
			add(Severity::WARNING, "0/18", entity, "Member of a cluster, but no announcement type supported - the service can never be interrupted");
	}

	// --- FIG 0/19 ---
	for(const asw_clusters_t::value_type& kv : ensemble.asw_clusters) {
		uint8_t cid = kv.first;
		const FIC_ASW_CLUSTER& ac = kv.second;
		QString entity = "Cluster " + ClusterName(cid);

		if(cid == 0x00)
			add(Severity::ERROR, "0/19", entity, "Cluster Id 0x00 shall not be used");
		if((ac.asw_flags & ALARM_FLAG) && cid != ALARM_CLUSTER)
			add(Severity::ERROR, "0/19", entity, "Alarm announcement on a cluster other than 0xFF - Cluster Id 0xFF shall be used exclusively for all alarm announcements");
		if(cid == ALARM_CLUSTER && (ac.asw_flags & ~ALARM_FLAG))
			add(Severity::ERROR, "0/19", entity, "Non-alarm type(s) on the alarm cluster: " + TypesString(ac.asw_flags & ~ALARM_FLAG));
		if(cid == ALARM_CLUSTER && ac.IsActive() && !ensemble.al_flag)
			add(Severity::ERROR, "0/19", entity, "Alarm announcement signalled while the Al flag of FIG 0/0 is 0 - receivers ignore it");
		if(ac.rfa_set)
			add(Severity::WARNING, "0/19", entity, "Rfa bit (b6) set: the Region flag of EN 300 401 V1.x, removed in V2 - its Region Id byte was skipped");
		if(ac.lost)
			add(Severity::ERROR, "0/19", entity, "Announcement no longer signalled, without its end (ASw flags all zero, ten times per second for two seconds)");

		if(!ac.IsActive())
			continue;

		if(!subchids.empty() && !subchids.count(ac.subchid))
			add(Severity::ERROR, "0/19", entity, "SubChId " + QString::number(ac.subchid) + " is not in the sub-channel organisation (FIG 0/1)");

		if(cid != ALARM_CLUSTER) {
			std::vector<const SERVICE_ENTRY*> members = ClusterMembers(cid);
			if(members.empty()) {
				add(Severity::WARNING, "0/19", entity, "Announcement on a cluster no service belongs to (FIG 0/18) - no receiver switches to it");
			} else {
				uint16_t supported = 0;
				for(const SERVICE_ENTRY *m : members)
					supported |= m->asu_flags;
				uint16_t unsupported = ac.asw_flags & ~supported;
				if(unsupported)
					add(Severity::WARNING, "0/19", entity, "Type(s) supported by no service of the cluster (FIG 0/18): " + TypesString(unsupported));
			}
		}

		if(since.count(cid) && since[cid].msecsTo(now) > LONG_ANNOUNCEMENT_MS)
			add(Severity::INFO, "0/19", entity, "Announcement in progress for more than " + Duration(LONG_ANNOUNCEMENT_MS));
	}

	// --- FIG 0/25 ---
	for(const fic_oe_announcement_supports_t::value_type& kv : oe_supports) {
		const FIC_OE_ANNOUNCEMENT_SUPPORT& s = kv.second;
		QString label = ServiceName(s.sid);
		QString entity = "SId " + Hex(s.sid, 4).mid(2) + (label.isEmpty() ? "" : " " + label);
		if(s.asu_flags & ALARM_FLAG)
			add(Severity::ERROR, "0/25", entity, "ASu flag b0 (Alarm) set - it shall be 0: alarm announcements are enabled ensemble-wide by the Al flag of FIG 0/0");
		for(const FIC_OE_ANNOUNCEMENT_TARGET& t : s.targets) {
			if(t.oe_cid == ALARM_CLUSTER)
				add(Severity::ERROR, "0/25", entity, "OE Cluster Id 0xFF signalled - it is pre-defined and shall not be signalled in FIG 0/25");
			if(t.eid == ensemble.eid)
				add(Severity::WARNING, "0/25", entity, "Target EId " + Hex(t.eid, 4).mid(2) + " is the tuned ensemble - its announcements are signalled by FIG 0/18");
		}
	}

	// --- FIG 0/26 ---
	for(const fic_oe_asw_clusters_t::value_type& kv : oe_clusters) {
		const FIC_OE_ASW_CLUSTER& ac = kv.second;
		QString entity = "OE cluster " + ClusterName(ac.oe_cid);

		if((ac.asw_flags & ALARM_FLAG) && ac.oe_cid != ALARM_CLUSTER)
			add(Severity::ERROR, "0/26", entity, "Alarm announcement on an OE cluster other than 0xFF - OE Cluster Id 0xFF shall be used exclusively for all alarm announcements");
		if(ac.oe_cid == ALARM_CLUSTER && (ac.asw_flags & ~ALARM_FLAG))
			add(Severity::ERROR, "0/26", entity, "Non-alarm type(s) on the alarm OE cluster: " + TypesString(ac.asw_flags & ~ALARM_FLAG));
		if(ac.oe_cid == ALARM_CLUSTER && ac.IsActive() && !ensemble.al_flag)
			add(Severity::ERROR, "0/26", entity, "Alarm announcement signalled while the Al flag of FIG 0/0 is 0 - receivers ignore it");
		if((ac.asw_flags & ALARM_FLAG) && ac.scids != 0)
			add(Severity::ERROR, "0/26", entity, "SCIdS " + QString::number(ac.scids) + " for an alarm announcement - it shall be 0");
		if(ac.lost)
			add(Severity::ERROR, "0/26", entity, "Announcement no longer signalled, without its end (ASw flags all zero, ten times per second for two seconds)");

		if(!ac.IsActive())
			continue;

		if(ac.oe_cid != ALARM_CLUSTER) {
			bool referenced = false;
			for(const fic_oe_announcement_supports_t::value_type& s : oe_supports)
				for(const FIC_OE_ANNOUNCEMENT_TARGET& t : s.second.targets)
					referenced |= t.oe_cid == ac.oe_cid;
			if(!referenced)
				add(Severity::WARNING, "0/26", entity, "Announcement on an OE cluster no service refers to (FIG 0/25) - no receiver switches to it");
		}

		if(oe_since.count(kv.first) && oe_since[kv.first].msecsTo(now) > LONG_ANNOUNCEMENT_MS)
			add(Severity::INFO, "0/26", entity, "Announcement in progress for more than " + Duration(LONG_ANNOUNCEMENT_MS));
	}

	// what isn't found anymore is kept, as past (e.g. the announcement it was about has ended)
	for(std::map<QString, FINDING>::value_type& kv : findings)
		if(!present.count(kv.first))
			kv.second.present = false;
}


// --- display -----------------------------------------------------------------
void EDIScopeQtAnnouncementsWindow::Refresh() {
	// --- summary ---
	if(!ensemble_known) {
		summary_label->setText("Waiting for the ensemble...");
	} else {
		int with_support = 0, primaries = 0;
		for(const SERVICE_ENTRY& s : services) {
			if(!s.primary)
				continue;
			primaries++;
			with_support += s.asu_flags != 0;
		}
		int active = 0;
		for(const asw_clusters_t::value_type& kv : ensemble.asw_clusters)
			active += kv.second.IsActive();
		for(const fic_oe_asw_clusters_t::value_type& kv : oe_clusters)
			active += kv.second.IsActive();
		int errors = 0, warnings = 0;
		for(const std::map<QString, FINDING>::value_type& kv : findings) {
			if(!kv.second.present)
				continue;
			errors += kv.second.severity == Severity::ERROR;
			warnings += kv.second.severity == Severity::WARNING;
		}

		QString al = ensemble.al_flag ? "✓ enabled" : "— disabled";
		QString checks = QString("✗ %1 &nbsp;▲ %2").arg(errors).arg(warnings);
		if(errors)
			checks = "<span style=\"color: " + EDIScopeQtTheme::error.name() + ";\"><b>" + checks + "</b></span>";
		else if(warnings)
			checks = "<span style=\"color: " + EDIScopeQtTheme::warning.name() + ";\">" + checks + "</span>";
		summary_label->setText(QString("Alarm (Al flag): %1 &nbsp;·&nbsp; Services with support: %2/%3 &nbsp;·&nbsp; In progress: <b>%4</b> &nbsp;·&nbsp; Checks: %5")
				.arg(al).arg(with_support).arg(primaries).arg(active).arg(checks));
	}

	RefreshClusters();
	RefreshSupport();
	RefreshHistory();
	RefreshChecks();
}

void EDIScopeQtAnnouncementsWindow::RefreshClusters() {
	QDateTime now = QDateTime::currentDateTime();

	// --- this ensemble ---
	clusters_view->setRowCount((int) ensemble.asw_clusters.size());
	int row = 0;
	for(const asw_clusters_t::value_type& kv : ensemble.asw_clusters) {
		uint8_t cid = kv.first;
		const FIC_ASW_CLUSTER& ac = kv.second;

		SetCell(clusters_view, row, 0, ClusterName(cid), true);
		if(ac.lost)
			SetColoredCell(clusters_view, row, 1, "✗ Lost", EDIScopeQtTheme::error, true);
		else if(ac.IsActive())
			SetColoredCell(clusters_view, row, 1, "▶ In progress", EDIScopeQtTheme::warning, true);
		else
			SetColoredCell(clusters_view, row, 1, "■ Ended", EDIScopeQtTheme::text_muted, false);
		SetCell(clusters_view, row, 2, ac.asw_flags ? TypesString(ac.asw_flags) : "—");
		SetCell(clusters_view, row, 3, QString::number(ac.subchid), true);
		SetCell(clusters_view, row, 4, SubchannelService(ac.subchid));
		SetCell(clusters_view, row, 5, ac.new_flag ? "yes" : "no");
		bool has_since = ac.IsActive() && since.count(cid);
		SetCell(clusters_view, row, 6, has_since ? Time(since.at(cid)) : QString(), true);
		SetCell(clusters_view, row, 7, has_since ? Duration(since.at(cid).msecsTo(now)) : QString(), true);

		QString members;
		if(cid == ALARM_CLUSTER) {
			members = ensemble.al_flag ? "all services (Al flag set)" : "none (Al flag not set)";
		} else {
			for(const SERVICE_ENTRY *m : ClusterMembers(cid))
				members += (members.isEmpty() ? "" : ", ") + QString::fromStdString(m->label);
			if(members.isEmpty())
				members = "none";
		}
		SetCell(clusters_view, row, 8, members);
		row++;
	}
	SizeOnce(clusters_view);

	// --- other ensembles ---
	oe_clusters_view->setRowCount((int) oe_clusters.size());
	row = 0;
	for(const fic_oe_asw_clusters_t::value_type& kv : oe_clusters) {
		const FIC_OE_ASW_CLUSTER& ac = kv.second;
		bool alarm = ac.oe_cid == ALARM_CLUSTER;

		SetCell(oe_clusters_view, row, 0, ClusterName(ac.oe_cid), true);
		if(ac.lost)
			SetColoredCell(oe_clusters_view, row, 1, "✗ Lost", EDIScopeQtTheme::error, true);
		else if(ac.IsActive())
			SetColoredCell(oe_clusters_view, row, 1, "▶ In progress", EDIScopeQtTheme::warning, true);
		else
			SetColoredCell(oe_clusters_view, row, 1, "■ Ended", EDIScopeQtTheme::text_muted, false);
		SetCell(oe_clusters_view, row, 2, ac.asw_flags ? TypesString(ac.asw_flags) : "—");
		SetCell(oe_clusters_view, row, 3, (alarm ? "EId " : "SId ") + Hex(ac.target_sid, 4).mid(2), true);
		SetCell(oe_clusters_view, row, 4, QString::number(ac.scids), true);
		SetCell(oe_clusters_view, row, 5, ac.new_flag ? "yes" : "no");
		bool has_since = ac.IsActive() && oe_since.count(kv.first);
		SetCell(oe_clusters_view, row, 6, has_since ? Time(oe_since.at(kv.first)) : QString(), true);
		SetCell(oe_clusters_view, row, 7, has_since ? Duration(oe_since.at(kv.first).msecsTo(now)) : QString(), true);

		QString members;
		if(alarm) {
			members = ensemble.al_flag ? "all services (Al flag set)" : "none (Al flag not set)";
		} else {
			for(const fic_oe_announcement_supports_t::value_type& s : oe_supports)
				for(const FIC_OE_ANNOUNCEMENT_TARGET& t : s.second.targets) {
					if(t.oe_cid != ac.oe_cid)
						continue;
					QString label = ServiceName(s.first);
					members += (members.isEmpty() ? "" : ", ") + (label.isEmpty() ? "SId " + Hex(s.first, 4).mid(2) : label) +
							" (EId " + Hex(t.eid, 4).mid(2) + ")";
				}
			if(members.isEmpty())
				members = "none";
		}
		SetCell(oe_clusters_view, row, 8, members);
		row++;
	}
	SizeOnce(oe_clusters_view);
}

void EDIScopeQtAnnouncementsWindow::RefreshSupport() {
	// --- FIG 0/18: every service, those without support too ---
	std::vector<const SERVICE_ENTRY*> primaries;
	for(const SERVICE_ENTRY& s : services)
		if(s.primary)
			primaries.push_back(&s);
	std::sort(primaries.begin(), primaries.end(), [](const SERVICE_ENTRY *a, const SERVICE_ENTRY *b) {return a->sid < b->sid;});

	support_view->setRowCount((int) primaries.size());
	for(size_t i = 0; i < primaries.size(); i++) {
		const SERVICE_ENTRY& s = *primaries[i];
		int row = (int) i;
		SetCell(support_view, row, 0, QString::fromStdString(StringTools::SidToHex(s.sid)), true);
		SetCell(support_view, row, 1, QString::fromStdString(s.label));
		if(s.asu_flags)
			SetCell(support_view, row, 2, TypesString(s.asu_flags));
		else
			SetColoredCell(support_view, row, 2, "— none", EDIScopeQtTheme::text_muted, false);
		QString clusters;
		for(uint8_t cid : s.cids)
			clusters += (clusters.isEmpty() ? "" : ", ") + Hex(cid, 2);
		SetCell(support_view, row, 3, clusters, true);
	}
	SizeOnce(support_view);

	// --- FIG 0/25 ---
	oe_support_view->setRowCount((int) oe_supports.size());
	int row = 0;
	for(const fic_oe_announcement_supports_t::value_type& kv : oe_supports) {
		const FIC_OE_ANNOUNCEMENT_SUPPORT& s = kv.second;
		SetCell(oe_support_view, row, 0, Hex(s.sid, 4).mid(2), true);
		SetCell(oe_support_view, row, 1, ServiceName(s.sid));
		SetCell(oe_support_view, row, 2, s.asu_flags ? TypesString(s.asu_flags) : "— none");
		QString targets;
		for(const FIC_OE_ANNOUNCEMENT_TARGET& t : s.targets)
			targets += (targets.isEmpty() ? "" : ", ") + Hex(t.oe_cid, 2) + " @ " + Hex(t.eid, 4).mid(2);
		SetCell(oe_support_view, row, 3, targets, true);
		row++;
	}
	SizeOnce(oe_support_view);
}

void EDIScopeQtAnnouncementsWindow::RefreshHistory() {
	// the most recent first; a refresh only when something was added (keeps the selection/scrolling)
	if(history_shown_serial == history_serial)
		return;
	history_shown_serial = history_serial;

	history_view->setRowCount((int) history.size());
	for(size_t i = 0; i < history.size(); i++) {
		const EVENT& e = history[history.size() - 1 - i];
		int row = (int) i;
		SetCell(history_view, row, 0, Time(e.time), true);
		SetCell(history_view, row, 1, e.fig, true);
		SetCell(history_view, row, 2, e.cluster, true);
		if(e.event.startsWith("✗"))
			SetColoredCell(history_view, row, 3, e.event, EDIScopeQtTheme::error, true);
		else if(e.event.startsWith("▶"))
			SetColoredCell(history_view, row, 3, e.event, EDIScopeQtTheme::warning, true);
		else
			SetCell(history_view, row, 3, e.event);
		SetCell(history_view, row, 4, e.types);
		SetCell(history_view, row, 5, e.where, true);
		SetCell(history_view, row, 6, e.carried_by);
		SetCell(history_view, row, 7, Duration(e.duration_ms), true);
		SetCell(history_view, row, 8, e.note);
	}
	SizeOnce(history_view);
}

void EDIScopeQtAnnouncementsWindow::RefreshChecks() {
	// present findings first, by severity; then the past ones
	std::vector<const FINDING*> list;
	for(const std::map<QString, FINDING>::value_type& kv : findings)
		list.push_back(&kv.second);
	std::stable_sort(list.begin(), list.end(), [](const FINDING *a, const FINDING *b) {
		if(a->present != b->present)
			return a->present;
		if(a->severity != b->severity)
			return a->severity > b->severity;
		return a->first_seen < b->first_seen;
	});

	checks_view->setRowCount((int) list.size());
	for(size_t i = 0; i < list.size(); i++) {
		const FINDING& f = *list[i];
		int row = (int) i;
		if(f.present)
			SetColoredCell(checks_view, row, 0, SeverityText(f.severity), SeverityColor(f.severity), f.severity != Severity::INFO);
		else
			SetColoredCell(checks_view, row, 0, SeverityText(f.severity), EDIScopeQtTheme::text_muted, false);
		SetCell(checks_view, row, 1, f.fig, true);
		SetCell(checks_view, row, 2, f.entity);
		SetCell(checks_view, row, 3, f.text)->setToolTip(f.text);
		SetCell(checks_view, row, 4, Time(f.first_seen), true);
		SetCell(checks_view, row, 5, Time(f.last_seen), true);
		if(f.present)
			SetCell(checks_view, row, 6, "● Present");
		else
			SetColoredCell(checks_view, row, 6, "○ Past", EDIScopeQtTheme::text_muted, false);
	}
	if(checks_view->rowCount() && !checks_view->property("sized").toBool()) {
		checks_view->resizeColumnsToContents();
		checks_view->setColumnWidth(3, std::min(checks_view->columnWidth(3), 640));
		checks_view->setProperty("sized", true);
	}

	// the tab title shows the present errors/warnings, so they are seen from the other tabs
	int count = 0;
	for(const FINDING *f : list)
		count += f->present && f->severity != Severity::INFO;
	tabs->setTabText(tabs->indexOf(checks_view), count ? QString("Checks (%1)").arg(count) : "Checks");
}

void EDIScopeQtAnnouncementsWindow::ExportHistory() {
	QString name = "announcements-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".csv";
	QString filename = QFileDialog::getSaveFileName(this, "Export History", QDir::home().filePath(name), "CSV files (*.csv);;All files (*)");
	if(filename.isEmpty())
		return;

	QFile file(filename);
	if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		QMessageBox::critical(this, "Export History", "Cannot write \"" + filename + "\":\n" + file.errorString());
		return;
	}

	auto quote = [](const QString& value) {
		QString v = value;
		v.replace("\"", "\"\"");
		return "\"" + v + "\"";
	};

	QTextStream out(&file);
	out << "Date,Time,FIG,Cluster,Event,Types,SubChId / Target,Carried by,Duration (s),Note\n";
	for(const EVENT& e : history) {
		// without the state symbol, for spreadsheets
		QString event = e.event;
		if(event.size() > 2 && event[1] == ' ')
			event = event.mid(2);
		out << e.time.toString("yyyy-MM-dd") << "," << e.time.toString("HH:mm:ss") << "," << quote(e.fig) << "," << quote(e.cluster) << ","
				<< quote(event) << "," << quote(e.types) << "," << quote(e.where) << "," << quote(e.carried_by) << ","
				<< (e.duration_ms >= 0 ? QString::number(e.duration_ms / 1000) : QString()) << "," << quote(e.note) << "\n";
	}
}
