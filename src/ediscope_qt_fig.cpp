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

#include "ediscope_qt_fig.h"
#include "ediscope_qt_theme.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

#include "tools.h"


static QTableWidget* CreateTable(const QStringList& headers, QWidget *parent) {
	QTableWidget *table = new QTableWidget(0, headers.size(), parent);
	table->setHorizontalHeaderLabels(headers);
	table->verticalHeader()->hide();
	table->verticalHeader()->setDefaultSectionSize(22);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setShowGrid(false);
	table->setAlternatingRowColors(true);
	table->setWordWrap(false);
	table->horizontalHeader()->setHighlightSections(false);
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	table->horizontalHeader()->setStretchLastSection(true);
	EDIScopeQtTheme::MakeTableCopyable(table);
	return table;
}

static void SetCell(QTableWidget *table, int row, int column, const QString& text, bool mono = false) {
	QTableWidgetItem *item = new QTableWidgetItem(text);
	if(mono)
		item->setFont(EDIScopeQtTheme::MonoFont());
	table->setItem(row, column, item);
}

static QString Interval(double ms) {
	return QString::fromStdString(FIGAnalyzer::FormatInterval(ms));
}


// --- EDIScopeQtFIGHistogram -----------------------------------------------------------------
static const int HISTOGRAM_ROW_HEIGHT = 26;

EDIScopeQtFIGHistogram::EDIScopeQtFIGHistogram(QWidget *parent) : QWidget(parent), nominal_ms(0) {
	setMinimumSize(440, 60 + FIGHistogram::classes * HISTOGRAM_ROW_HEIGHT + 30);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
}

void EDIScopeQtFIGHistogram::SetHistogram(const FIGHistogram& histogram, double nominal_ms, const QString& title) {
	this->histogram = histogram;
	this->nominal_ms = nominal_ms;
	this->title = title;
	update();
}

void EDIScopeQtFIGHistogram::Clear() {
	histogram = FIGHistogram();
	nominal_ms = 0;
	title.clear();
	update();
}

void EDIScopeQtFIGHistogram::paintEvent(QPaintEvent* /*event*/) {
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);

	QFont title_font = font();
	title_font.setBold(true);
	QFont mono = EDIScopeQtTheme::MonoFont();
	QFontMetricsF mono_metrics(mono);

	painter.setPen(EDIScopeQtTheme::text_secondary);
	painter.setFont(title_font);
	painter.drawText(QRectF(0, 0, width(), 20), Qt::AlignLeft | Qt::AlignVCenter, title.isEmpty() ? "Intervals" : title);

	if(nominal_ms <= 0) {
		painter.setFont(font());
		painter.setPen(EDIScopeQtTheme::text_muted);
		painter.drawText(QRectF(0, 28, width(), 40), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, "Select a requirement to see the distribution of its intervals.");
		return;
	}

	uint64_t total = histogram.Total();
	const double label_width = 150;
	// wide enough for e.g. "123456  100.0 %"
	const double value_width = QFontMetricsF(EDIScopeQtTheme::MonoFont()).horizontalAdvance("123456  100.0 %") + 4;
	const double bar_x = label_width + 6;
	const double bar_width = std::max(20.0, width() - bar_x - value_width - 6);
	double y = 30;

	painter.setFont(mono);
	for(int c = 0; c < FIGHistogram::classes; c++) {
		// zone of the class, as for the verdicts
		FIGVerdict zone = c == 0 ? FIGVerdict::OK : (c <= 3 ? FIGVerdict::DEGRADED : FIGVerdict::FAIL);
		QColor color = EDIScopeQtFIGWindow::VerdictColor(zone);
		const char *mark = zone == FIGVerdict::OK ? "✓" : (zone == FIGVerdict::DEGRADED ? "~" : "✗");

		QString limit;
		if(c < FIGHistogram::classes - 1)
			limit = QString("≤ %1× %2").arg(FIGHistogram::limits[c]).arg(QString::fromStdString(FIGAnalyzer::FormatInterval(FIGHistogram::limits[c] * nominal_ms)));
		else
			limit = QString("> %1× %2").arg(FIGHistogram::limits[c - 1]).arg(QString::fromStdString(FIGAnalyzer::FormatInterval(FIGHistogram::limits[c - 1] * nominal_ms)));

		QRectF row(0, y, width(), HISTOGRAM_ROW_HEIGHT);
		painter.setPen(color);
		painter.drawText(QRectF(row.left(), row.top(), 16, row.height()), Qt::AlignLeft | Qt::AlignVCenter, mark);
		painter.setPen(EDIScopeQtTheme::text_secondary);
		painter.drawText(QRectF(row.left() + 16, row.top(), label_width - 16, row.height()), Qt::AlignLeft | Qt::AlignVCenter, limit);

		// bar track, and the bar proportional to the share of all intervals (at least visible if not empty)
		QRectF track(bar_x, row.top() + 6, bar_width, row.height() - 12);
		painter.setPen(Qt::NoPen);
		painter.setBrush(EDIScopeQtTheme::input_bg);
		painter.drawRoundedRect(track, 3, 3);
		uint64_t count = histogram.counts[c];
		if(count) {
			double share = (double) count / total;
			QRectF bar(track.left(), track.top(), std::max(3.0, track.width() * share), track.height());
			painter.setBrush(color);
			painter.drawRoundedRect(bar, 3, 3);
		}

		painter.setPen(count ? EDIScopeQtTheme::text : EDIScopeQtTheme::text_disabled);
		QString value = count ? QString("%1  %2 %").arg(count).arg(100.0 * count / total, 0, 'f', count * 1000 < total ? 2 : 1) : QString("0");
		painter.drawText(QRectF(bar_x + bar_width + 6, row.top(), value_width, row.height()), Qt::AlignRight | Qt::AlignVCenter, value);

		y += HISTOGRAM_ROW_HEIGHT;
	}

	painter.setFont(font());
	painter.setPen(EDIScopeQtTheme::text_muted);
	painter.drawText(QRectF(0, y + 4, width(), 40), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
			QString("%1 interval(s) between repetitions; the current one (since the last repetition) is not counted.").arg(total));
}


// --- EDIScopeQtFIGWindow -----------------------------------------------------------------
EDIScopeQtFIGWindow::EDIScopeQtFIGWindow(QWidget *parent, SnapshotProvider snapshot_provider) :
		QWidget(nullptr, Qt::Window), snapshot_provider(snapshot_provider), positioned(false) {
	main_window = parent;
	setWindowTitle("FIG Analysis");
	resize(1100, 760);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);

	// --- summary ---
	QHBoxLayout *header = new QHBoxLayout();
	summary_label = new QLabel(this);
	summary_label->setFont(EDIScopeQtTheme::MonoFont());
	summary_label->setTextFormat(Qt::RichText);
	header->addWidget(summary_label, 1);
	QPushButton *reset_button = new QPushButton("Reset", this);
	reset_button->setToolTip("Start the measurement again from now");
	connect(reset_button, &QPushButton::clicked, this, [this]() {
		emit ResetRequested();
		Refresh();
	});
	header->addWidget(reset_button);
	layout->addLayout(header);

	capacity_label = new QLabel(this);
	capacity_label->setWordWrap(true);
	EDIScopeQtTheme::SetRole(capacity_label, "muted");
	layout->addWidget(capacity_label);

	QTabWidget *tabs = new QTabWidget(this);
	layout->addWidget(tabs, 1);

	// --- "Repetition (ETSI)" tab ---
	QSplitter *splitter = new QSplitter(Qt::Vertical, tabs);
	splitter->setChildrenCollapsible(false);

	checks_view = CreateTable({"FIG", "Usage", "ETSI TS 103 176 requirement", "Measured", "Worst interval", "Verdict"}, splitter);
	checks_view->setSelectionMode(QAbstractItemView::SingleSelection);
	checks_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	connect(checks_view, &QTableWidget::itemSelectionChanged, this, [this]() {
		QList<QTableWidgetItem*> items = checks_view->selectedItems();
		if(!items.isEmpty()) {
			int row = items.first()->row();
			QString check = checks_view->item(row, 0)->text() + "|" + checks_view->item(row, 1)->text();
			if(check != selected_check)
				selected_entity.clear();
			selected_check = check;
		}
		RefreshDetail();
	});
	splitter->addWidget(checks_view);

	QWidget *detail = new QWidget(splitter);
	QVBoxLayout *detail_layout = new QVBoxLayout(detail);
	detail_layout->setContentsMargins(0, 6, 0, 0);
	detail_layout->setSpacing(6);
	detail_title = new QLabel("Select a requirement above to see its measurement in detail.", detail);
	EDIScopeQtTheme::SetRole(detail_title, "subtitle");
	detail_layout->addWidget(detail_title);
	QHBoxLayout *detail_row = new QHBoxLayout();
	detail_row->setSpacing(14);
	detail_stack = new QStackedWidget(detail);
	entities_view = CreateTable({"Entity", "Repetitions", "Min interval", "Avg interval", "Max interval", "Since last", "Verdict"}, detail_stack);
	entities_view->setSelectionMode(QAbstractItemView::SingleSelection);
	entities_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	detail_stack->addWidget(entities_view);
	check_events_view = CreateTable({"Time", "Entity", "Event", "Repetitions", "Span", "Max interval", "Verdict", "Note"}, detail_stack);
	check_events_view->setSelectionMode(QAbstractItemView::SingleSelection);
	check_events_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	detail_stack->addWidget(check_events_view);
	detail_row->addWidget(detail_stack, 1);

	histogram_view = new EDIScopeQtFIGHistogram(detail);
	detail_row->addWidget(histogram_view, 0, Qt::AlignTop);
	detail_layout->addLayout(detail_row, 1);

	// selecting an entity (or an event) shows its own histogram, instead of the requirement's
	connect(entities_view, &QTableWidget::itemSelectionChanged, this, [this]() {
		QList<QTableWidgetItem*> items = entities_view->selectedItems();
		selected_entity = items.isEmpty() ? QString() : entities_view->item(items.first()->row(), 0)->text();
		RefreshHistogram();
	});
	connect(check_events_view, &QTableWidget::itemSelectionChanged, this, [this]() {
		QList<QTableWidgetItem*> items = check_events_view->selectedItems();
		selected_entity = items.isEmpty() ? QString() : check_events_view->item(items.first()->row(), 0)->data(Qt::UserRole).toString();
		RefreshHistogram();
	});
	splitter->addWidget(detail);
	splitter->setSizes({330, 330});

	tabs->addTab(splitter, "Repetition (ETSI)");

	// --- "FIGs" tab ---
	figs_view = CreateTable({"FIG", "Description", "Count", "Rate", "Bytes", "Max interval", "Verdict"}, tabs);
	figs_view->setSelectionMode(QAbstractItemView::SingleSelection);
	figs_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	tabs->addTab(figs_view, "FIGs");

	// --- "Events" tab ---
	events_view = CreateTable({"Time", "FIG", "Entity", "Event", "Requirement", "Repetitions", "Span", "Max interval", "Verdict", "Note"}, tabs);
	events_view->setSelectionMode(QAbstractItemView::SingleSelection);
	events_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	tabs->addTab(events_view, "Events");

	refresh_timer.setInterval(1000);
	connect(&refresh_timer, &QTimer::timeout, this, &EDIScopeQtFIGWindow::Refresh);
}

void EDIScopeQtFIGWindow::closeEvent(QCloseEvent *event) {
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

void EDIScopeQtFIGWindow::showEvent(QShowEvent *event) {
	Refresh();
	refresh_timer.start();
	QWidget::showEvent(event);
}

void EDIScopeQtFIGWindow::hideEvent(QHideEvent *event) {
	refresh_timer.stop();
	QWidget::hideEvent(event);
}

void EDIScopeQtFIGWindow::AlignToParent() {
	if(positioned || !main_window)
		return;
	positioned = true;

	QRect parent_frame = main_window->frameGeometry();
	move(parent_frame.right() + 20, parent_frame.top());
}

QString EDIScopeQtFIGWindow::EventKey(const FIG_ANALYSIS_EVENT& e) {
	return QString::number(e.t_ms) + "|" + QString::fromStdString(e.entity + "|" + e.event);
}

QString EDIScopeQtFIGWindow::VerdictText(FIGVerdict verdict) {
	// a symbol as well as a colour, so the verdicts never rely on colour alone
	switch(verdict) {
	case FIGVerdict::OK:		return "✓ OK";
	case FIGVerdict::DEGRADED:	return "~ Degraded";
	case FIGVerdict::FAIL:		return "✗ Not compliant";
	case FIGVerdict::OBSERVING:	return "… Observing";
	default:					return "—";
	}
}

QColor EDIScopeQtFIGWindow::VerdictColor(FIGVerdict verdict) {
	switch(verdict) {
	case FIGVerdict::OK:		return EDIScopeQtTheme::ok;
	case FIGVerdict::DEGRADED:	return EDIScopeQtTheme::warning;
	case FIGVerdict::FAIL:		return EDIScopeQtTheme::error;
	default:					return EDIScopeQtTheme::text_muted;
	}
}

void EDIScopeQtFIGWindow::SetVerdictCell(QTableWidget *table, int row, int column, FIGVerdict verdict) {
	QTableWidgetItem *item = new QTableWidgetItem(VerdictText(verdict));
	item->setForeground(VerdictColor(verdict));
	if(verdict == FIGVerdict::FAIL || verdict == FIGVerdict::DEGRADED) {
		QFont font = item->font();
		font.setBold(true);
		item->setFont(font);
	}
	item->setData(Qt::UserRole, (int) verdict);	// sorts by severity
	table->setItem(row, column, item);
}

void EDIScopeQtFIGWindow::FillEvents(QTableWidget *table, const std::vector<FIG_ANALYSIS_EVENT>& events) {
	bool full = table->columnCount() == 10;	// the "Events" tab, with FIG and requirement columns
	table->setRowCount((int) events.size());
	for(size_t i = 0; i < events.size(); i++) {
		const FIG_ANALYSIS_EVENT& e = events[i];
		int row = (int) i, column = 0;
		SetCell(table, row, column++, QDateTime::fromSecsSinceEpoch(e.time).toString("HH:mm:ss"), true);
		table->item(row, 0)->setData(Qt::UserRole, EventKey(e));
		if(full)
			SetCell(table, row, column++, QString::fromStdString(e.fig));
		SetCell(table, row, column++, QString::fromStdString(e.entity), true);
		SetCell(table, row, column++, QString::fromStdString(e.event));
		if(full)
			SetCell(table, row, column++, QString::fromStdString(e.requirement));
		SetCell(table, row, column++, QString::number(e.repetitions), true);
		SetCell(table, row, column++, Interval(e.span_ms), true);
		SetCell(table, row, column++, e.repetitions > 1 ? Interval(e.max_gap_ms) : QString(), true);
		SetVerdictCell(table, row, column++, e.complete || e.verdict == FIGVerdict::FAIL ? e.verdict : FIGVerdict::OBSERVING);
		SetCell(table, row, column++, QString::fromStdString(e.note.empty() ? (e.complete ? "" : "burst in progress") : e.note));
	}
}

void EDIScopeQtFIGWindow::Refresh() {
	if(!snapshot_provider)
		return;
	snapshot = snapshot_provider();

	// --- summary ---
	QString crc = QString::number(snapshot.fib_crc_errors);
	if(snapshot.fib_crc_errors)
		crc = "<span style=\"color: " + EDIScopeQtTheme::warning.name() + ";\">" + crc + "</span>";
	summary_label->setText(QString("Observed: <b>%1</b> &nbsp;·&nbsp; FIBs: %2 (CRC errors: %3) &nbsp;·&nbsp; FIC load: %4 % &nbsp;·&nbsp; Services: %5, components: %6")
			.arg(QString::fromStdString(StringTools::MsToTimecode((long int) snapshot.observed_ms)))
			.arg(snapshot.fibs).arg(crc)
			.arg(snapshot.fic_load * 100, 0, 'f', 1)
			.arg(snapshot.services).arg(snapshot.components));

	// ETSI TS 103 176 clause 4: what the FIC capacity allows
	QString capacity;
	if(snapshot.components <= 20)
		capacity = "the nominal repetition rates are achievable (up to 20 DAB+ services)";
	else if(snapshot.components < 40)
		capacity = "the repetition rates can be expected to decline towards half of the nominal rates (around 40 components)";
	else if(snapshot.components < 60)
		capacity = "the repetition rates can be expected to be about half of the nominal rates, declining towards one third (around 60 components)";
	else
		capacity = "the repetition rates can be expected to be about one third of the nominal rates - the minimum allowed";
	capacity_label->setText("ETSI TS 103 176 V2.6.1 clause 4 and table 1. Verdicts: ✓ at the nominal rate, ~ slower but not below one third of it "
			"(the minimum allowed), ✗ below. With " + QString::number(snapshot.components) + " service components, " + capacity + ".");

	// --- checks ---
	checks_view->setRowCount((int) snapshot.checks.size());
	int select_row = -1;
	for(size_t i = 0; i < snapshot.checks.size(); i++) {
		const FIG_ANALYSIS_CHECK& c = snapshot.checks[i];
		int row = (int) i;
		SetCell(checks_view, row, 0, QString::fromStdString(c.fig), true);
		SetCell(checks_view, row, 1, QString::fromStdString(c.usage));
		SetCell(checks_view, row, 2, QString::fromStdString(c.requirement));
		SetCell(checks_view, row, 3, c.burst ? QString("%1 event(s)").arg(c.events) : QString("%1 entit%2").arg(c.entities.size()).arg(c.entities.size() == 1 ? "y" : "ies"));
		SetCell(checks_view, row, 4, c.worst_gap_ms > 0 ? Interval(c.worst_gap_ms) : QString(), true);
		SetVerdictCell(checks_view, row, 5, c.verdict);
		if(QString::fromStdString(c.fig) + "|" + QString::fromStdString(c.usage) == selected_check)
			select_row = row;
	}
	checks_view->blockSignals(true);
	if(select_row != -1)
		checks_view->selectRow(select_row);
	else
		checks_view->clearSelection();
	checks_view->blockSignals(false);
	static bool sized = false;
	if(!sized && !snapshot.checks.empty()) {
		checks_view->resizeColumnsToContents();
		sized = true;
	}
	RefreshDetail();

	// --- FIGs ---
	figs_view->setRowCount((int) snapshot.figs.size());
	for(size_t i = 0; i < snapshot.figs.size(); i++) {
		const FIG_ANALYSIS_FIG& f = snapshot.figs[i];
		int row = (int) i;
		SetCell(figs_view, row, 0, QString::fromStdString(f.fig), true);
		SetCell(figs_view, row, 1, QString::fromStdString(f.description));
		SetCell(figs_view, row, 2, QString::number(f.count), true);
		SetCell(figs_view, row, 3, f.rate_per_s >= 1 ? QString::number(f.rate_per_s, 'f', 1) + " /s" : QString::number(f.rate_per_s * 60, 'f', 1) + " /min", true);
		SetCell(figs_view, row, 4, QString::fromStdString(StringTools::FormatByteSize(f.bytes)), true);
		SetCell(figs_view, row, 5, Interval(f.max_gap_ms), true);
		SetVerdictCell(figs_view, row, 6, f.verdict);
	}
	static bool figs_sized = false;
	if(!figs_sized && !snapshot.figs.empty()) {
		figs_view->resizeColumnsToContents();
		figs_sized = true;
	}

	// --- events ---
	FillEvents(events_view, snapshot.events);
}

void EDIScopeQtFIGWindow::RefreshDetail() {
	const FIG_ANALYSIS_CHECK *check = nullptr;
	for(const FIG_ANALYSIS_CHECK& c : snapshot.checks)
		if(QString::fromStdString(c.fig) + "|" + QString::fromStdString(c.usage) == selected_check)
			check = &c;

	if(!check) {
		detail_title->setText("Select a requirement above to see its measurement in detail.");
		entities_view->setRowCount(0);
		check_events_view->setRowCount(0);
		histogram_view->Clear();
		return;
	}

	detail_title->setText(QString::fromStdString(check->fig + " - " + check->usage + ": " + check->requirement) +
			(check->burst ? "  (each event is judged on its own burst)" : "  (interval between two repetitions of each entity)"));

	if(check->burst) {
		detail_stack->setCurrentIndex(1);
		std::vector<FIG_ANALYSIS_EVENT> events;
		for(const FIG_ANALYSIS_EVENT& e : snapshot.events)
			if(e.fig == check->fig && e.requirement == check->requirement)
				events.push_back(e);
		check_events_view->blockSignals(true);
		FillEvents(check_events_view, events);
		check_events_view->clearSelection();
		for(int row = 0; row < check_events_view->rowCount(); row++)
			if(check_events_view->item(row, 0)->data(Qt::UserRole).toString() == selected_entity)
				check_events_view->selectRow(row);
		check_events_view->blockSignals(false);
		RefreshHistogram();
		return;
	}

	detail_stack->setCurrentIndex(0);
	entities_view->blockSignals(true);
	entities_view->setRowCount((int) check->entities.size());
	for(size_t i = 0; i < check->entities.size(); i++) {
		const FIG_ANALYSIS_ENTITY& e = check->entities[i];
		int row = (int) i;
		SetCell(entities_view, row, 0, QString::fromStdString(e.name), true);
		SetCell(entities_view, row, 1, QString::number(e.count), true);
		SetCell(entities_view, row, 2, e.count > 1 ? Interval(e.min_gap_ms) : QString(), true);
		SetCell(entities_view, row, 3, e.count > 1 ? Interval(e.avg_gap_ms) : QString(), true);
		SetCell(entities_view, row, 4, Interval(e.max_gap_ms), true);
		SetCell(entities_view, row, 5, Interval(e.since_last_ms), true);
		SetVerdictCell(entities_view, row, 6, e.verdict);
	}
	entities_view->clearSelection();
	for(int row = 0; row < entities_view->rowCount(); row++)
		if(entities_view->item(row, 0)->text() == selected_entity)
			entities_view->selectRow(row);
	entities_view->blockSignals(false);
	RefreshHistogram();
	static bool entities_sized = false;
	if(!entities_sized && !check->entities.empty()) {
		entities_view->resizeColumnsToContents();
		entities_sized = true;
	}
}

void EDIScopeQtFIGWindow::RefreshHistogram() {
	const FIG_ANALYSIS_CHECK *check = nullptr;
	for(const FIG_ANALYSIS_CHECK& c : snapshot.checks)
		if(QString::fromStdString(c.fig) + "|" + QString::fromStdString(c.usage) == selected_check)
			check = &c;
	if(!check) {
		histogram_view->Clear();
		return;
	}

	QString check_name = QString::fromStdString(check->fig + " - " + check->usage);
	if(!selected_entity.isEmpty()) {
		if(check->burst) {
			for(const FIG_ANALYSIS_EVENT& e : snapshot.events)
				if(EventKey(e) == selected_entity) {
					histogram_view->SetHistogram(e.histogram, e.nominal_ms, "Intervals - " + QString::fromStdString(e.entity + ", " + e.event));
					return;
				}
		} else {
			for(const FIG_ANALYSIS_ENTITY& e : check->entities)
				if(QString::fromStdString(e.name) == selected_entity) {
					histogram_view->SetHistogram(e.histogram, check->nominal_ms, "Intervals - " + QString::fromStdString(e.name));
					return;
				}
		}
	}
	histogram_view->SetHistogram(check->histogram, check->nominal_ms, "Intervals - " + check_name + (check->burst ? " (all events)" : " (all entities)"));
}
