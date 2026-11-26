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

#include "ediscope_qt_rdns.h"
#include "ediscope_qt_sls.h"
#include "ediscope_qt_theme.h"

#include <set>

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "tools.h"


static const int LOGO_THUMB_SIZE = 96;
static const int ROLE_FULL_IMAGE = Qt::UserRole;
static const int ROLE_SORT = Qt::UserRole + 1;

// a table item sorted by a numeric key (ROLE_SORT) instead of its text - e.g. "128 kBit/s"
class EDIScopeQtNumericItem : public QTableWidgetItem {
public:
	EDIScopeQtNumericItem(const QString& text, int sort_key) : QTableWidgetItem(text) {setData(ROLE_SORT, sort_key);}
	bool operator<(const QTableWidgetItem& other) const override {return data(ROLE_SORT).toInt() < other.data(ROLE_SORT).toInt();}
};

static QLabel* CreateMessage(const QString& text, QWidget *parent) {
	QLabel *label = new QLabel(text, parent);
	label->setWordWrap(true);
	label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	label->setContentsMargins(4, 14, 4, 4);
	label->setStyleSheet("color: #9aa3ad; font-size: 13px;");
	return label;
}

// re-indents an XML document, one element per line - returned unchanged if it isn't well-formed
static QString IndentXML(const QString& xml) {
	QString result;
	QXmlStreamReader reader(xml);
	QXmlStreamWriter writer(&result);
	writer.setAutoFormatting(true);
	writer.setAutoFormattingIndent(2);
	while(!reader.atEnd()) {
		reader.readNext();
		if(reader.isWhitespace())
			continue;	// replaced by the writer's own indentation
		writer.writeCurrentToken(reader);
	}
	return reader.hasError() ? xml : result;
}

static QLabel* CreateTitle(QWidget *parent) {
	QLabel *label = new QLabel(parent);
	label->setWordWrap(true);
	label->setStyleSheet("font-weight: 700; font-size: 14px; color: #f3f5f7;");
	return label;
}


// --- EDIScopeQtRDNSWindow -----------------------------------------------------------------
EDIScopeQtRDNSWindow::EDIScopeQtRDNSWindow(QWidget *parent) : QWidget(parent, Qt::Tool), popup_item(nullptr), positioned(false) {
	setWindowTitle("RadioDNS");
	resize(900, 560);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(8);

	QHBoxLayout *toolbar = new QHBoxLayout();
	QPushButton *test_all_button = new QPushButton("Test all services", this);
	connect(test_all_button, &QPushButton::clicked, this, [this]() {
		for(const SERVICE_ENTRY& service : std::vector<SERVICE_ENTRY>(services))
			RequestTest(service);
	});
	toolbar->addWidget(test_all_button);
	toolbar->addStretch(1);
	layout->addLayout(toolbar);

	main_stack = new QStackedWidget(this);
	layout->addWidget(main_stack, 1);
	main_stack->addWidget(CreateMessage("No audio service known yet on this ensemble.", main_stack));

	QSplitter *splitter = new QSplitter(Qt::Horizontal, main_stack);
	splitter->setChildrenCollapsible(false);
	main_stack->addWidget(splitter);

	services_list = new QTableWidget(0, 2, splitter);
	services_list->setHorizontalHeaderLabels({"Service", "Status"});
	services_list->verticalHeader()->hide();
	services_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
	services_list->setSelectionMode(QAbstractItemView::SingleSelection);
	services_list->setSelectionBehavior(QAbstractItemView::SelectRows);
	services_list->setShowGrid(false);
	services_list->setAlternatingRowColors(true);
	services_list->horizontalHeader()->setHighlightSections(false);
	services_list->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	services_list->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	connect(services_list, &QTableWidget::itemSelectionChanged, this, &EDIScopeQtRDNSWindow::ShowDetailForSelection);
	splitter->addWidget(services_list);

	// --- detail pane (right side) ---
	QWidget *detail = new QWidget(splitter);
	QVBoxLayout *detail_layout = new QVBoxLayout(detail);
	detail_layout->setContentsMargins(10, 0, 0, 0);
	detail_layout->setSpacing(10);

	QHBoxLayout *header = new QHBoxLayout();
	detail_title = CreateTitle(detail);
	header->addWidget(detail_title, 1);
	test_selected_button = new QPushButton("Test this service", detail);
	connect(test_selected_button, &QPushButton::clicked, this, [this]() {
		int row = SelectedRow();
		if(row >= 0 && row < (int) services.size())
			RequestTest(services[row]);
	});
	header->addWidget(test_selected_button);
	detail_layout->addLayout(header);

	QGridLayout *grid = new QGridLayout();
	grid->setHorizontalSpacing(10);
	grid->setVerticalSpacing(4);
	static const char *stage_titles[] = {"FQDN:", "GCC:", "DNS CNAME:", "DNS SRV (_radioepg._tcp):", "HTTP (SI.xml):"};
	for(int i = 0; i < 5; i++) {
		QLabel *title = new QLabel(stage_titles[i], detail);
		title->setStyleSheet("font-weight: 600; color: #9aa3ad;");
		title->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		grid->addWidget(title, i, 0);

		stage_values[i] = new QLabel(detail);
		stage_values[i]->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		stage_values[i]->setWordWrap(true);
		stage_values[i]->setTextInteractionFlags(Qt::TextSelectableByMouse);
		grid->addWidget(stage_values[i], i, 1);
	}
	stage_values[0]->setFont(EDIScopeQtTheme::MonoFont());
	grid->setColumnStretch(1, 1);
	detail_layout->addLayout(grid);

	// the tabs sit in an area that always takes the remaining height, so the trace above keeps its
	// compact size whether they are shown (resolved) or not
	QWidget *result_area = new QWidget(detail);
	QVBoxLayout *result_layout = new QVBoxLayout(result_area);
	result_layout->setContentsMargins(0, 0, 0, 0);
	result_tabs = new QTabWidget(result_area);
	result_layout->addWidget(result_tabs, 1);
	detail_layout->addWidget(result_area, 1);
	splitter->addWidget(detail);
	splitter->setSizes({280, 620});
	splitter->setStretchFactor(1, 1);

	// --- "Info" tab ---
	QScrollArea *info_scroll = new QScrollArea(result_tabs);
	info_scroll->setWidgetResizable(true);
	info_scroll->setFrameShape(QFrame::NoFrame);
	QWidget *info = new QWidget(info_scroll);
	QVBoxLayout *info_layout = new QVBoxLayout(info);
	info_layout->setContentsMargins(10, 10, 10, 10);
	info_layout->setSpacing(4);
	info_placeholder = CreateMessage("Nothing decoded from the SI.xml for this service yet.", info);
	info_provider_name = CreateTitle(info);
	info_provider_desc = new QLabel(info);
	info_provider_logos = CreateLogoList(info);
	info_service_name = CreateTitle(info);
	info_service_name->setContentsMargins(0, 12, 0, 0);
	info_service_desc = new QLabel(info);
	for(QLabel *desc : {info_provider_desc, info_service_desc}) {
		desc->setWordWrap(true);
		desc->setTextInteractionFlags(Qt::TextSelectableByMouse);
	}
	info_layout->addWidget(info_placeholder);
	info_layout->addWidget(info_provider_name);
	info_layout->addWidget(info_provider_desc);
	info_layout->addWidget(info_provider_logos);
	info_layout->addWidget(info_service_name);
	info_layout->addWidget(info_service_desc);
	info_layout->addStretch(1);
	info_scroll->setWidget(info);
	result_tabs->addTab(info_scroll, "Info");

	// --- "Logos" tab --- (this service's own)
	logos_stack = new QStackedWidget(result_tabs);
	logos_stack->addWidget(CreateMessage("No logo found for this service in the SI.xml.", logos_stack));
	logos_list = CreateLogoList(logos_stack);
	logos_stack->addWidget(logos_list);
	result_tabs->addTab(logos_stack, "Logos");

	// --- "Bearers" tab ---
	bearers_view = new QTableWidget(0, 4, result_tabs);
	bearers_view->setHorizontalHeaderLabels({"Bearer", "Bitrate", "Cost", "Type"});
	bearers_view->verticalHeader()->hide();
	bearers_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
	bearers_view->setSelectionMode(QAbstractItemView::SingleSelection);
	bearers_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	bearers_view->setShowGrid(false);
	bearers_view->setAlternatingRowColors(true);
	bearers_view->setSortingEnabled(true);	// orderable by clicking the header, same as the services table
	bearers_view->horizontalHeader()->setHighlightSections(false);
	bearers_view->horizontalHeader()->setStretchLastSection(true);
	bearers_view->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(bearers_view, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
		// only over a stream bearer (a plain http(s):// URL) - "dab:"/"fm:" ones aren't addresses
		// of anything that could be opened or usefully copied as a link
		// "Copy" for any bearer; "Open" only for a stream (a plain http(s):// URL) - "dab:"/"fm:"
		// ones aren't addresses of anything that could be opened
		QTableWidgetItem *item = bearers_view->itemAt(pos);
		if(!item)
			return;
		QTableWidgetItem *id_item = bearers_view->item(item->row(), 0);
		QString id = id_item ? id_item->text() : QString();
		QStringList row_cells;
		for(int column = 0; column < bearers_view->columnCount(); column++) {
			QTableWidgetItem *it = bearers_view->item(item->row(), column);
			row_cells << (it ? it->text() : QString());
		}
		bearers_view->selectRow(item->row());

		QMenu menu(this);
		if(IsStreamBearer(id.toStdString()))
			menu.addAction("Open", this, [id]() {QDesktopServices::openUrl(QUrl(id));});
		menu.addAction("Copy", this, [id]() {QApplication::clipboard()->setText(id);});
		menu.addAction("Copy row", this, [row_cells]() {QApplication::clipboard()->setText(row_cells.join('\t'));});
		menu.exec(bearers_view->viewport()->mapToGlobal(pos));
	});
	result_tabs->addTab(bearers_view, "Bearers");

	// --- "SI.xml" tab ---
	xml_view = new QPlainTextEdit(result_tabs);
	xml_view->setReadOnly(true);
	xml_view->setFont(EDIScopeQtTheme::MonoFont());
	xml_view->setLineWrapMode(QPlainTextEdit::NoWrap);
	xml_view->setStyleSheet("QPlainTextEdit { background: #12151a; border: none; }");
	result_tabs->addTab(xml_view, "SI.xml");

	popup = new EDIScopeQtImagePopup(this);
	popup->hide();

	Clear();
}

QListWidget* EDIScopeQtRDNSWindow::CreateLogoList(QWidget *parent) {
	// logos flow left to right and wrap, each with its caption below
	QListWidget *list = new QListWidget(parent);
	list->setViewMode(QListView::IconMode);
	list->setFlow(QListView::LeftToRight);
	list->setWrapping(true);
	list->setResizeMode(QListView::Adjust);
	list->setMovement(QListView::Static);
	list->setIconSize(QSize(LOGO_THUMB_SIZE, LOGO_THUMB_SIZE));
	list->setGridSize(QSize(LOGO_THUMB_SIZE + 60, LOGO_THUMB_SIZE + 50));
	list->setSpacing(6);
	list->setSelectionMode(QAbstractItemView::NoSelection);
	list->setFocusPolicy(Qt::NoFocus);
	list->setWordWrap(true);
	list->setFrameShape(QFrame::NoFrame);
	list->setStyleSheet("QListWidget { background: transparent; } QListWidget::item { color: #9aa3ad; }");
	list->setMouseTracking(true);
	list->viewport()->setMouseTracking(true);
	return list;
}

void EDIScopeQtRDNSWindow::FillLogoList(QListWidget *list, const std::vector<RDNS_XML_LOGO>& logos) {
	popup->hide();
	popup_item = nullptr;
	list->clear();
	list->viewport()->removeEventFilter(this);
	list->viewport()->installEventFilter(this);	// shows a logo at full size on hover

	for(const RDNS_XML_LOGO& logo : logos) {
		QImage image;
		if(!logo.data.empty())
			image.loadFromData(logo.data.data(), (int) logo.data.size());

		std::string caption = logo.type;
		if(logo.width > 0 && logo.height > 0)
			caption += " " + std::to_string(logo.width) + "x" + std::to_string(logo.height);
		if(!logo.data.empty())
			caption += "\n" + StringTools::FormatByteSize(logo.data.size());

		QListWidgetItem *item = new QListWidgetItem(QString::fromStdString(caption));
		item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
		if(!image.isNull()) {
			item->setIcon(QIcon(QPixmap::fromImage(image.scaled(LOGO_THUMB_SIZE, LOGO_THUMB_SIZE, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
			item->setData(ROLE_FULL_IMAGE, image);
		} else {
			// not fetched, or not decodable
			item->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
			item->setToolTip("Logo not available:\n" + QString::fromStdString(logo.url));
		}
		list->addItem(item);
	}

	// a logo list on the Info tab sizes itself to its content, instead of scrolling inside a scroll area
	if(list == info_provider_logos) {
		list->setVisible(!logos.empty());
		list->setFixedHeight(logos.empty() ? 0 : list->gridSize().height() + 2 * list->spacing() + 4);
	}
}

bool EDIScopeQtRDNSWindow::eventFilter(QObject *watched, QEvent *event) {
	for(QListWidget *list : {logos_list, info_provider_logos}) {
		if(watched != list->viewport())
			continue;

		if(event->type() == QEvent::MouseMove) {
			QListWidgetItem *item = list->itemAt(static_cast<QMouseEvent*>(event)->position().toPoint());
			QVariant image = item ? item->data(ROLE_FULL_IMAGE) : QVariant();
			if(!image.isValid()) {
				popup->hide();
				popup_item = nullptr;
			} else if(item != popup_item || !popup->isVisible()) {
				popup->ShowAt(QCursor::pos(), image.value<QImage>(), QString());
				popup_item = item;
			} else {
				popup->FollowCursor(QCursor::pos());
			}
		} else if(event->type() == QEvent::Leave || event->type() == QEvent::Hide) {
			popup->hide();
			popup_item = nullptr;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void EDIScopeQtRDNSWindow::closeEvent(QCloseEvent *event) {
	// the application is quitting (the main window is gone): let it close for real
	if(!parentWidget() || !parentWidget()->isVisible()) {
		event->accept();
		return;
	}

	// just hidden, so its content survives; the main window un-toggles its button
	popup->hide();
	hide();
	event->ignore();
	emit Closed();
}

void EDIScopeQtRDNSWindow::AlignToParent() {
	// only once, the first time the window is shown - afterwards it stays wherever the user left it
	if(positioned || !parentWidget())
		return;
	positioned = true;

	QRect parent_frame = parentWidget()->window()->frameGeometry();
	move(parent_frame.right() + 20, parent_frame.top());
}

bool EDIScopeQtRDNSWindow::IsStreamBearer(const std::string& id) {
	return id.compare(0, 7, "http://") == 0 || id.compare(0, 8, "https://") == 0;
}

int EDIScopeQtRDNSWindow::SelectedRow() const {
	QList<QTableWidgetItem*> items = services_list->selectedItems();
	return items.isEmpty() ? -1 : items.first()->row();
}

void EDIScopeQtRDNSWindow::RefreshRowStatus(int row) {
	if(row < 0 || row >= (int) services.size())
		return;
	std::pair<int,int> key(services[row].sid, services[row].scids);

	// a symbol as well as a colour, so the states never rely on colour alone
	QString status;
	QColor color = EDIScopeQtTheme::text_muted;
	std::map<std::pair<int,int>,bool>::const_iterator pending_it = pending.find(key);
	std::map<std::pair<int,int>,RDNS_RESULT>::const_iterator result_it = results.find(key);
	if(pending_it != pending.end() && pending_it->second) {
		status = "… Testing";
	} else if(result_it != results.end()) {
		const RDNS_RESULT& result = result_it->second;
		if(result.FullyResolved()) {
			status = "✓ OK";
			color = EDIScopeQtTheme::ok;
		} else if(result.http.attempted) {
			status = "✗ Blocked: HTTP";
			color = EDIScopeQtTheme::error;
		} else if(result.srv.attempted) {
			status = "✗ Blocked: DNS SRV";
			color = EDIScopeQtTheme::error;
		} else if(result.cname.attempted) {
			status = "✗ Blocked: DNS CNAME";
			color = EDIScopeQtTheme::error;
		}
	}

	QTableWidgetItem *item = new QTableWidgetItem(status);
	item->setForeground(color);
	services_list->setItem(row, 1, item);
}

void EDIScopeQtRDNSWindow::ShowDetailForSelection() {
	int row = SelectedRow();
	if(row < 0 || row >= (int) services.size()) {
		detail_title->clear();
		test_selected_button->setEnabled(false);
		for(QLabel *value : stage_values)
			value->clear();
		result_tabs->setVisible(false);
		return;
	}

	const SERVICE_ENTRY& service = services[row];
	detail_title->setText(QString::fromStdString(service.label + "  (SId " + StringTools::SidToHex(service.sid) + ", SCIdS " + std::to_string(service.scids) + ")"));

	std::pair<int,int> key(service.sid, service.scids);
	std::map<std::pair<int,int>,bool>::const_iterator pending_it = pending.find(key);
	bool is_pending = pending_it != pending.end() && pending_it->second;
	test_selected_button->setEnabled(!is_pending);

	std::map<std::pair<int,int>,RDNS_RESULT>::const_iterator result_it = results.find(key);
	if(result_it == results.end()) {
		stage_values[0]->setText(is_pending ? "(testing…)" : "(not tested yet)");
		for(int i = 1; i < 5; i++)
			stage_values[i]->clear();
		result_tabs->setVisible(false);
		return;
	}

	const RDNS_RESULT& result = result_it->second;
	stage_values[0]->setText(result.fqdn.empty() ? "(couldn't be built)" : QString::fromStdString(result.fqdn));
	stage_values[1]->setText(result.gcc.empty() ? "(unknown)" : QString::fromStdString(result.gcc));

	const RDNS_STAGE *stages[] = {&result.cname, &result.srv, &result.http};
	for(int i = 0; i < 3; i++) {
		const RDNS_STAGE& stage = *stages[i];
		QLabel *value = stage_values[2 + i];
		if(!stage.attempted) {
			value->setText("(not reached)");
			value->setStyleSheet("color: #9aa3ad;");
		} else {
			value->setText(QString(stage.success ? "✓ " : "✗ ") + QString::fromStdString(stage.detail));
			value->setStyleSheet(stage.success ? "color: #34d399;" : "color: #f87171;");
		}
	}

	result_tabs->setVisible(result.FullyResolved());
	if(result.FullyResolved())
		PopulateResultTabs(result);
}

void EDIScopeQtRDNSWindow::PopulateResultTabs(const RDNS_RESULT& result) {
	auto best_name = [](const RDNS_XML_SERVICE& s) {
		return !s.long_name.empty() ? s.long_name : !s.medium_name.empty() ? s.medium_name : s.short_name;
	};
	auto best_description = [](const RDNS_XML_SERVICE& s) {
		return !s.long_description.empty() ? s.long_description : s.short_description;
	};

	// --- Info ---
	bool have_provider_info = !result.si_provider.IsEmpty();
	bool have_service_info = result.si_service_matched && !result.si_service.IsEmpty();

	info_placeholder->setVisible(!result.si_parsed || (!have_provider_info && !have_service_info));

	info_provider_name->setVisible(have_provider_info);
	info_provider_desc->setVisible(have_provider_info && !best_description(result.si_provider).empty());
	if(have_provider_info) {
		info_provider_name->setText("Service Provider: " + QString::fromStdString(best_name(result.si_provider)));
		info_provider_desc->setText(QString::fromStdString(best_description(result.si_provider)));
	}
	FillLogoList(info_provider_logos, result.si_provider.logos);

	info_service_name->setVisible(have_service_info);
	info_service_desc->setVisible(have_service_info && !best_description(result.si_service).empty());
	if(have_service_info) {
		info_service_name->setText("This service: " + QString::fromStdString(best_name(result.si_service)));
		info_service_desc->setText(QString::fromStdString(best_description(result.si_service)));
	}

	// --- Logos --- (this service's own - the serviceProvider's are on the Info tab)
	FillLogoList(logos_list, result.si_service.logos);
	logos_stack->setCurrentIndex(result.si_service.logos.empty() ? 0 : 1);

	// --- Bearers ---
	bearers_view->setSortingEnabled(false);	// while filling, or rows end up scattered
	bearers_view->setRowCount((int) result.si_service.bearers.size());
	for(size_t i = 0; i < result.si_service.bearers.size(); i++) {
		const RDNS_XML_BEARER& bearer = result.si_service.bearers[i];
		bearers_view->setItem((int) i, 0, new QTableWidgetItem(QString::fromStdString(bearer.id)));
		bearers_view->setItem((int) i, 1, new EDIScopeQtNumericItem(bearer.bitrate.empty() ? QString() : QString::fromStdString(bearer.bitrate) + " kBit/s",
				bearer.bitrate.empty() ? -1 : atoi(bearer.bitrate.c_str())));
		bearers_view->setItem((int) i, 2, new QTableWidgetItem(QString::fromStdString(bearer.cost)));
		bearers_view->setItem((int) i, 3, new QTableWidgetItem(QString::fromStdString(bearer.mime)));
	}
	bearers_view->setSortingEnabled(true);
	bearers_view->resizeColumnsToContents();

	// --- SI.xml --- (re-indented: servers often send it on a single line)
	xml_view->setPlainText(result.si_parsed ? IndentXML(QString::fromStdString(result.si_xml)) :
			QString::fromStdString(result.si_xml.empty() ? "" : result.si_xml + "\n\n(couldn't be parsed as well-formed XML)"));
}

void EDIScopeQtRDNSWindow::RequestTest(const SERVICE_ENTRY& service) {
	std::pair<int,int> key(service.sid, service.scids);
	if(pending[key])
		return;
	pending[key] = true;

	for(size_t row = 0; row < services.size(); row++)
		if(services[row].sid == service.sid && services[row].scids == service.scids)
			RefreshRowStatus((int) row);
	ShowDetailForSelection();

	emit TestRequested(service.sid, service.scids, service.ecc, service.eid);
}

void EDIScopeQtRDNSWindow::SetServices(const std::vector<SERVICE_ENTRY>& new_services) {
	// keep the selection on the same service, if it's still there
	int prev_sid = -1, prev_scids = -1;
	int prev_row = SelectedRow();
	if(prev_row >= 0 && prev_row < (int) services.size()) {
		prev_sid = services[prev_row].sid;
		prev_scids = services[prev_row].scids;
	}

	services = new_services;

	// drop stale results/pending state for services no longer listed (e.g. a hot mux reconfiguration)
	std::set<std::pair<int,int>> current_keys;
	for(const SERVICE_ENTRY& s : services)
		current_keys.insert(std::make_pair(s.sid, s.scids));
	for(std::map<std::pair<int,int>,RDNS_RESULT>::iterator it = results.begin(); it != results.end();)
		it = current_keys.count(it->first) ? std::next(it) : results.erase(it);
	for(std::map<std::pair<int,int>,bool>::iterator it = pending.begin(); it != pending.end();)
		it = current_keys.count(it->first) ? std::next(it) : pending.erase(it);

	services_list->blockSignals(true);
	services_list->clearSelection();
	services_list->setRowCount((int) services.size());
	int restore_row = -1;
	for(size_t i = 0; i < services.size(); i++) {
		services_list->setItem((int) i, 0, new QTableWidgetItem(QString::fromStdString(services[i].label)));
		RefreshRowStatus((int) i);
		if(services[i].sid == prev_sid && services[i].scids == prev_scids)
			restore_row = (int) i;
	}

	main_stack->setCurrentIndex(services.empty() ? 0 : 1);

	// nothing selected before (e.g. the window just opened): the first row, rather than requiring
	// an extra click before its trace can be seen
	if(restore_row == -1 && !services.empty())
		restore_row = 0;
	if(restore_row != -1)
		services_list->selectRow(restore_row);
	services_list->blockSignals(false);

	ShowDetailForSelection();
}

void EDIScopeQtRDNSWindow::UpdateResult(int sid, int scids, const RDNS_RESULT& result) {
	std::pair<int,int> key(sid, scids);
	results[key] = result;
	pending[key] = false;

	for(size_t row = 0; row < services.size(); row++)
		if(services[row].sid == sid && services[row].scids == scids)
			RefreshRowStatus((int) row);

	int row = SelectedRow();
	if(row >= 0 && row < (int) services.size() && services[row].sid == sid && services[row].scids == scids)
		ShowDetailForSelection();
}

void EDIScopeQtRDNSWindow::Clear() {
	services.clear();
	services_list->setRowCount(0);
	results.clear();
	pending.clear();
	main_stack->setCurrentIndex(0);
	ShowDetailForSelection();
}
