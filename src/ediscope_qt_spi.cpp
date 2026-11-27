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

#include "ediscope_qt_spi.h"
#include "ediscope_qt_sls.h"
#include "ediscope_qt_theme.h"

#include <algorithm>
#include <ctime>
#include <iterator>

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QStyle>
#include <QTabWidget>
#include <QVBoxLayout>

#include "tools.h"


static const int CARD_WIDTH = 300;
static const int LOGO_BOX_WIDTH = 86;
static const int LOGO_THUMB_SIZE = 72;
static const char *PROPERTY_FULL_IMAGE = "fullImage";

static QLabel* CreateMessage(const QString& text, QWidget *parent) {
	QLabel *label = new QLabel(text, parent);
	label->setWordWrap(true);
	label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	label->setContentsMargins(14, 14, 14, 14);
	EDIScopeQtTheme::SetRole(label, "message");
	return label;
}

// a scroll area showing a grid of cards, top-left aligned
static QScrollArea* CreateCardsArea(QGridLayout*& grid, QWidget *parent) {
	QScrollArea *scroll = new QScrollArea(parent);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	QWidget *content = new QWidget(scroll);
	QVBoxLayout *outer = new QVBoxLayout(content);
	outer->setContentsMargins(10, 10, 10, 10);
	grid = new QGridLayout();
	grid->setSpacing(10);
	grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	outer->addLayout(grid);
	outer->addStretch(1);
	scroll->setWidget(content);
	return scroll;
}


// --- EDIScopeQtSPITimeline -----------------------------------------------------------------
static const int TIMELINE_RULER_WIDTH = 50;
static const int TIMELINE_BLOCK_WIDTH = 480;
static const int TIMELINE_MARGIN = 10;

constexpr double EDIScopeQtSPITimeline::px_per_min;

EDIScopeQtSPITimeline::EDIScopeQtSPITimeline(QWidget *parent) : QWidget(parent) {
	setFixedSize(TIMELINE_RULER_WIDTH + 10 + TIMELINE_BLOCK_WIDTH + 2 * TIMELINE_MARGIN, (int) (24 * 60 * px_per_min) + 2 * TIMELINE_MARGIN);
}

void EDIScopeQtSPITimeline::SetBlocks(const std::vector<Block>& blocks) {
	this->blocks = blocks;
	update();
}

void EDIScopeQtSPITimeline::paintEvent(QPaintEvent* /*event*/) {
	QPainter painter(this);
	painter.translate(TIMELINE_MARGIN, TIMELINE_MARGIN);

	QFont small_font = font();
	small_font.setPointSizeF(small_font.pointSizeF() * 0.8);
	QFontMetricsF small_metrics(small_font);

	// hour ruler
	painter.setFont(small_font);
	for(int h = 0; h < 24; h++) {
		double y = h * 60 * px_per_min;
		painter.setPen(EDIScopeQtTheme::text_muted);
		painter.drawText(QRectF(0, y - small_metrics.height() / 2, TIMELINE_RULER_WIDTH - 6, small_metrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
				QString::asprintf("%02d:00", h));
		painter.setPen(EDIScopeQtTheme::border);
		painter.drawLine(QPointF(TIMELINE_RULER_WIDTH, y), QPointF(TIMELINE_RULER_WIDTH + TIMELINE_BLOCK_WIDTH, y));
	}

	// programmes
	QFont name_font = font();
	name_font.setBold(true);
	QFontMetricsF name_metrics(name_font);
	painter.setRenderHint(QPainter::Antialiasing, true);
	for(const Block& block : blocks) {
		int duration = block.duration_min >= 0 ? block.duration_min : 15;
		QRectF rect(TIMELINE_RULER_WIDTH + 10, block.start_min * px_per_min, TIMELINE_BLOCK_WIDTH, std::max(20.0, duration * px_per_min));

		QPainterPath path;
		path.addRoundedRect(rect.adjusted(0, 0.5, 0, -0.5), 6, 6);
		painter.fillPath(path, EDIScopeQtTheme::timeline_block);
		painter.setPen(EDIScopeQtTheme::timeline_border);
		painter.drawPath(path);

		painter.save();
		painter.setClipRect(rect);
		QRectF text_rect = rect.adjusted(8, 3, -8, -3);
		painter.setPen(EDIScopeQtTheme::text_strong);
		painter.setFont(name_font);
		painter.drawText(QRectF(text_rect.topLeft(), QSizeF(text_rect.width(), name_metrics.height())), Qt::AlignLeft | Qt::AlignTop,
				name_metrics.elidedText(QString::fromStdString(block.name), Qt::ElideRight, text_rect.width()));

		QString time_text = QString::asprintf("%02d:%02d", block.start_min / 60, block.start_min % 60);
		if(block.duration_min >= 0)
			time_text += " (" + QString::number(block.duration_min) + " min)";
		painter.setPen(EDIScopeQtTheme::timeline_time);
		painter.setFont(small_font);
		painter.drawText(QRectF(text_rect.left(), text_rect.top() + name_metrics.height(), text_rect.width(), small_metrics.height()), Qt::AlignLeft | Qt::AlignTop, time_text);
		painter.restore();
	}
}


// --- EDIScopeQtSPIWindow -----------------------------------------------------------------
EDIScopeQtSPIWindow::EDIScopeQtSPIWindow(QWidget *parent) : QWidget(nullptr, Qt::Window), positioned(false) {
	main_window = parent;
	setWindowTitle("SPI / EPG");

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);

	QTabWidget *tabs = new QTabWidget(this);
	layout->addWidget(tabs);

	// --- "Services" tab (SI) ---
	services_stack = new QStackedWidget(tabs);
	services_message = CreateMessage(QString(), services_stack);
	services_stack->addWidget(services_message);
	SetSPIStatus(-1);
	services_stack->addWidget(CreateCardsArea(services_grid, services_stack));
	tabs->addTab(services_stack, "Services");

	// --- "Programme" tab (PI) - one service's day at a time ---
	QWidget *programme_page = new QWidget(tabs);
	QVBoxLayout *programme_layout = new QVBoxLayout(programme_page);
	programme_layout->setContentsMargins(10, 10, 10, 10);
	programme_selector = new QComboBox(programme_page);
	programme_selector->setMinimumWidth(320);
	programme_selector->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	connect(programme_selector, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EDIScopeQtSPIWindow::RedrawProgrammeTimeline);
	QHBoxLayout *programme_toolbar = new QHBoxLayout();
	programme_toolbar->addWidget(programme_selector);
	programme_toolbar->addStretch(1);
	programme_layout->addLayout(programme_toolbar);

	programme_stack = new QStackedWidget(programme_page);
	programme_stack->addWidget(CreateMessage("No programme schedule decoded yet for this ensemble.", programme_stack));
	QScrollArea *programme_scroll = new QScrollArea(programme_stack);
	programme_scroll->setFrameShape(QFrame::NoFrame);
	programme_timeline = new EDIScopeQtSPITimeline(programme_scroll);
	programme_scroll->setWidget(programme_timeline);
	programme_stack->addWidget(programme_scroll);
	programme_layout->addWidget(programme_stack, 1);
	tabs->addTab(programme_page, "Programme");

	// --- "Groups" tab (GI) ---
	groups_stack = new QStackedWidget(tabs);
	groups_stack->addWidget(CreateMessage("No programme group decoded yet for this ensemble.", groups_stack));
	groups_stack->addWidget(CreateCardsArea(groups_grid, groups_stack));
	tabs->addTab(groups_stack, "Groups");

	// --- "XML" tab ---
	QWidget *xml_page = new QWidget(tabs);
	QVBoxLayout *xml_layout = new QVBoxLayout(xml_page);
	xml_layout->setContentsMargins(10, 10, 10, 10);
	xml_selector = new QComboBox(xml_page);
	xml_selector->addItems({"Services", "Programme", "Groups"});
	connect(xml_selector, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EDIScopeQtSPIWindow::RedrawXML);
	QHBoxLayout *xml_toolbar = new QHBoxLayout();
	xml_toolbar->addWidget(xml_selector);
	xml_toolbar->addStretch(1);
	xml_layout->addLayout(xml_toolbar);
	xml_view = new QPlainTextEdit(xml_page);
	xml_view->setReadOnly(true);
	xml_view->setFont(EDIScopeQtTheme::MonoFont());
	xml_view->setLineWrapMode(QPlainTextEdit::NoWrap);
	xml_view->setObjectName("xmlView");
	xml_layout->addWidget(xml_view, 1);
	tabs->addTab(xml_page, "XML");

	popup = new EDIScopeQtImagePopup(this);
	popup->hide();

	// wide enough for 4 cards per row, tall enough for about 3 rows of them
	resize(20 + cards_per_row * CARD_WIDTH + (cards_per_row - 1) * 10 + 60, 860);	// 4 cards per row

	Clear();
}

void EDIScopeQtSPIWindow::closeEvent(QCloseEvent *event) {
	// the application is quitting (the main window is gone): let it close for real
	if(!main_window || !main_window->isVisible()) {
		event->accept();
		return;
	}

	// just hidden, so its content survives; the main window un-toggles its button
	popup->hide();
	hide();
	event->ignore();
	emit Closed();
}

void EDIScopeQtSPIWindow::AlignToParent() {
	// only once, the first time the window is shown - afterwards it stays wherever the user left it
	if(positioned || !main_window)
		return;
	positioned = true;

	EDIScopeQtTheme::CenterOnWindow(this, main_window);
}

std::string EDIScopeQtSPIWindow::PickName(const std::map<std::string,std::string>& names) {
	if(names.empty())
		return "";

	std::map<std::string,std::string>::const_iterator it = names.find("");
	return it != names.end() ? it->second : names.begin()->second;
}

std::string EDIScopeQtSPIWindow::BestName(const std::map<std::string,std::string>& long_names, const std::map<std::string,std::string>& medium_names, const std::map<std::string,std::string>& short_names) {
	std::string name = PickName(long_names);
	if(name.empty())
		name = PickName(medium_names);
	if(name.empty())
		name = PickName(short_names);
	return name;
}

std::string EDIScopeQtSPIWindow::FormatDABBearer(bool has_dab_bearer, int ecc, int eid, uint32_t sid, bool sid_32bit, int scids) {
	if(!has_dab_bearer)
		return "(no DAB bearer)";

	return "dab:" +
			StringTools::IntToHex(ecc, 2) + "." +
			StringTools::IntToHex(eid, 4) + "." +
			StringTools::IntToHex((int) sid, sid_32bit ? 8 : 4) + "." +
			std::to_string(scids);
}

std::string EDIScopeQtSPIWindow::FormatLogoCaption(const SPI_LOGO& logo, size_t data_size) {
	// logo_colour_square/logo_colour_rectangle have a mandatory fixed size (ETSI TS 102 818
	// clause 5.8) and must NOT carry width/height attributes - only logo_unrestricted signals its own
	std::string dims;
	switch(logo.type) {
	case SPI_LOGO::LOGO_TYPE_COLOUR_SQUARE:
		dims = "square 32x32";
		break;
	case SPI_LOGO::LOGO_TYPE_COLOUR_RECTANGLE:
		dims = "rectangle 112x32";
		break;
	case SPI_LOGO::LOGO_TYPE_UNRESTRICTED:
		dims = "unrestricted" + (logo.width > 0 && logo.height > 0 ? " " + std::to_string(logo.width) + "x" + std::to_string(logo.height) : "");
		break;
	default:
		dims = "logo";
		break;
	}

	if(data_size)
		dims += "\n" + StringTools::FormatByteSize(data_size);
	return dims;
}

std::string EDIScopeQtSPIWindow::FindServiceName(uint32_t sid) const {
	for(const SPI_SERVICE& service : last_si.services) {
		if(!service.has_dab_bearer || service.sid != sid)
			continue;
		std::string name = BestName(service.long_names, service.medium_names, service.short_names);
		if(!name.empty())
			return name;
	}

	std::map<uint32_t,std::string>::const_iterator it = known_service_names.find(sid);
	if(it != known_service_names.end())
		return it->second;

	return "SId " + StringTools::SidToHex((int) sid);
}

void EDIScopeQtSPIWindow::SetKnownServiceNames(const std::map<uint32_t,std::string>& names) {
	known_service_names = names;
	RebuildProgrammeSelector();
}

void EDIScopeQtSPIWindow::SetSPIStatus(int subchid) {
	if(subchid < 0)
		services_message->setText("No SPI service detected on this ensemble (yet).");
	else
		services_message->setText(QString(
				"SPI service found on SubChId %1 - waiting for its Service Information.\n\n"
				"The SPI carousel repeats slowly: the services and their logos can take a minute or more to arrive, "
				"the programme schedules even longer.").arg(subchid));
}

void EDIScopeQtSPIWindow::ClearGrid(QGridLayout *grid) {
	while(QLayoutItem *item = grid->takeAt(0)) {
		if(item->widget())
			item->widget()->deleteLater();
		delete item;
	}
}

QWidget* EDIScopeQtSPIWindow::BuildLogos(const std::vector<SPI_LOGO>& logos, const std::map<std::string,std::vector<uint8_t>>& logo_data, QWidget *parent) {
	QWidget *box = new QWidget(parent);
	QGridLayout *grid = new QGridLayout(box);
	grid->setContentsMargins(0, 4, 0, 0);
	grid->setSpacing(6);
	grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);

	int i = 0;
	for(const SPI_LOGO& logo : logos) {
		std::map<std::string,std::vector<uint8_t>>::const_iterator data_it = logo_data.find(logo.content_name);
		size_t data_size = data_it != logo_data.end() ? data_it->second.size() : 0;

		QImage image;
		if(data_size)
			image.loadFromData(data_it->second.data(), (int) data_size);

		QFrame *logo_box = new QFrame(box);
		logo_box->setObjectName("spiLogo");
		logo_box->setFixedWidth(LOGO_BOX_WIDTH);
		QVBoxLayout *logo_layout = new QVBoxLayout(logo_box);
		logo_layout->setContentsMargins(6, 6, 6, 6);
		logo_layout->setSpacing(2);

		QLabel *image_label = new QLabel(logo_box);
		image_label->setAlignment(Qt::AlignCenter);
		if(!image.isNull()) {
			QImage thumb = image.width() > LOGO_THUMB_SIZE || image.height() > LOGO_THUMB_SIZE ?
					image.scaled(LOGO_THUMB_SIZE, LOGO_THUMB_SIZE, Qt::KeepAspectRatio, Qt::SmoothTransformation) : image;
			image_label->setPixmap(QPixmap::fromImage(thumb));
			// shown at its real, undownscaled size on hover
			logo_box->setProperty(PROPERTY_FULL_IMAGE, image);
			// right-click: save the logo file as received, or copy it
			QByteArray file_data((const char*) data_it->second.data(), (int) data_size);
			QString file_name = QString::fromStdString(logo.content_name);
			QString mime = QString::fromStdString(logo.mime);
			logo_box->setContextMenuPolicy(Qt::CustomContextMenu);
			connect(logo_box, &QWidget::customContextMenuRequested, this, [this, logo_box, file_data, image, file_name, mime](const QPoint& pos) {
				popup->hide();
				EDIScopeQtTheme::ShowImageFileMenu(this, logo_box->mapToGlobal(pos), file_data, image, file_name, mime, "logo");
			});
			logo_box->installEventFilter(this);
			logo_box->setMouseTracking(true);	// so the popup follows the pointer
			image_label->setMouseTracking(true);
		} else {
			image_label->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(32, 32));
			logo_box->setToolTip("Logo not received (yet): " + QString::fromStdString(logo.content_name));
		}
		logo_layout->addWidget(image_label);

		QLabel *caption = new QLabel(QString::fromStdString(FormatLogoCaption(logo, data_size)), logo_box);
		caption->setAlignment(Qt::AlignCenter);
		caption->setWordWrap(true);	// e.g. "rectangle" / "112x32" on two lines rather than cut off
		EDIScopeQtTheme::SetRole(caption, "small");
		logo_layout->addWidget(caption);

		grid->addWidget(logo_box, i / logos_per_row, i % logos_per_row);
		i++;
	}
	return box;
}

QWidget* EDIScopeQtSPIWindow::BuildCard(const std::string& name, const std::string& subtitle, bool subtitle_mono, const std::vector<SPI_LOGO>& logos,
		const std::map<std::string,std::vector<uint8_t>>& logo_data) {
	QFrame *card = new QFrame();
	card->setObjectName("panel");
	card->setFixedWidth(CARD_WIDTH);
	QVBoxLayout *layout = new QVBoxLayout(card);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(6);

	QLabel *name_label = new QLabel(QString::fromStdString(name), card);
	name_label->setWordWrap(true);
	EDIScopeQtTheme::SetRole(name_label, "title");
	layout->addWidget(name_label);

	if(!subtitle.empty()) {
		QLabel *subtitle_label = new QLabel(QString::fromStdString(subtitle), card);
		EDIScopeQtTheme::SetRole(subtitle_label, "note");
		subtitle_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
		if(subtitle_mono)
			subtitle_label->setFont(EDIScopeQtTheme::MonoFont());
		layout->addWidget(subtitle_label);
	}

	if(!logos.empty())
		layout->addWidget(BuildLogos(logos, logo_data, card));
	layout->addStretch(1);
	return card;
}

bool EDIScopeQtSPIWindow::eventFilter(QObject *watched, QEvent *event) {
	QVariant image = watched->property(PROPERTY_FULL_IMAGE);
	if(image.isValid()) {
		switch(event->type()) {
		case QEvent::Enter:
			popup->ShowAt(QCursor::pos(), image.value<QImage>(), QString());
			break;
		case QEvent::MouseMove:
			if(popup->isVisible())
				popup->FollowCursor(QCursor::pos());
			break;
		case QEvent::Leave:
		case QEvent::Hide:
			popup->hide();
			break;
		default:
			break;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void EDIScopeQtSPIWindow::RebuildProgrammeSelector() {
	QString prev_selection = programme_selector->currentText();

	programme_selector->blockSignals(true);
	programme_selector->clear();
	int select_index = -1, i = 0;
	for(const std::pair<const std::pair<uint32_t,std::string>,SPI_SCHEDULE>& kv : last_schedules) {
		const std::string& date_key = kv.first.second;
		QString label = QString::fromStdString(FindServiceName(kv.first.first) + (date_key.empty() ? "" : " - " + date_key));
		programme_selector->addItem(label);
		if(label == prev_selection)
			select_index = i;
		i++;
	}
	if(!last_schedules.empty())
		programme_selector->setCurrentIndex(select_index >= 0 ? select_index : 0);
	programme_selector->blockSignals(false);

	programme_selector->setVisible(!last_schedules.empty());
	RedrawProgrammeTimeline();
}

void EDIScopeQtSPIWindow::RedrawProgrammeTimeline() {
	int selected = programme_selector->currentIndex();
	if(last_schedules.empty() || selected < 0) {
		programme_stack->setCurrentIndex(0);
		return;
	}
	programme_stack->setCurrentIndex(1);

	std::map<std::pair<uint32_t,std::string>,SPI_SCHEDULE>::const_iterator it = last_schedules.begin();
	std::advance(it, std::min(selected, (int) last_schedules.size() - 1));
	const SPI_SCHEDULE& schedule = it->second;

	std::vector<EDIScopeQtSPITimeline::Block> blocks;
	for(const SPI_PROGRAMME& prog : schedule.programmes) {
		if(!prog.time.valid)
			continue;

		// apply LTO the same way FICDecoder::ConvertDateTimeToString does, to position the block
		// at the same local time shown in its label
		struct tm local_tm = prog.time.utc.dt;
		if(prog.time.lto != FIC_ENSEMBLE::lto_none)
			local_tm.tm_min += prog.time.lto * 30;
		local_tm.tm_isdst = -1;
		if(mktime(&local_tm) == (time_t) -1)
			continue;

		EDIScopeQtSPITimeline::Block block;
		block.start_min = local_tm.tm_hour * 60 + local_tm.tm_min;
		block.duration_min = prog.time.duration_seconds >= 0 ? prog.time.duration_seconds / 60 : -1;
		block.name = BestName(prog.long_names, prog.medium_names, prog.short_names);
		if(block.name.empty())
			block.name = "(unnamed programme)";
		blocks.push_back(block);
	}
	programme_timeline->SetBlocks(blocks);
}

void EDIScopeQtSPIWindow::RedrawXML() {
	switch(xml_selector->currentIndex()) {
	case 0:
		xml_view->setPlainText(QString::fromStdString(SPIDecoder::ToXML(last_si)));
		break;
	case 1: {
		std::string xml;
		for(const std::pair<const std::pair<uint32_t,std::string>,SPI_SCHEDULE>& kv : last_schedules)
			xml += SPIDecoder::ToXML(kv.second) + "\n";
		xml_view->setPlainText(QString::fromStdString(xml));
		break;
	}
	case 2:
		xml_view->setPlainText(QString::fromStdString(SPIDecoder::ToXML(last_gi)));
		break;
	}
}

void EDIScopeQtSPIWindow::Update(const SPI_SERVICE_INFORMATION& info, const std::map<std::string,std::vector<uint8_t>>& logo_data) {
	last_si = info;

	popup->hide();
	ClearGrid(services_grid);
	services_stack->setCurrentIndex(info.services.empty() ? 0 : 1);
	int i = 0;
	for(const SPI_SERVICE& service : info.services) {
		std::string name = BestName(service.long_names, service.medium_names, service.short_names);
		if(name.empty())
			name = "(unnamed service)";
		services_grid->addWidget(BuildCard(name, FormatDABBearer(service.has_dab_bearer, service.ecc, service.eid, service.sid, service.sid_32bit, service.scids), true,
				service.logos, logo_data), i / cards_per_row, i % cards_per_row);
		i++;
	}

	RebuildProgrammeSelector();	// service names shown there come from here
	if(xml_selector->currentIndex() == 0)
		RedrawXML();
}

void EDIScopeQtSPIWindow::UpdateSchedule(const SPI_SCHEDULE& schedule) {
	if(!schedule.has_scope_service)
		return;

	last_schedules[std::make_pair(schedule.sid, SPIDecoder::FormatDate(schedule.scope_start))] = schedule;
	RebuildProgrammeSelector();

	if(xml_selector->currentIndex() == 1)
		RedrawXML();
}

void EDIScopeQtSPIWindow::UpdateGroupInformation(const SPI_GROUP_INFORMATION& info, const std::map<std::string,std::vector<uint8_t>>& logo_data) {
	static const std::map<int,std::string> type_names = {
		{SPI_PROGRAMME_GROUP::TYPE_SERIES, "series"},
		{SPI_PROGRAMME_GROUP::TYPE_SHOW, "show"},
		{SPI_PROGRAMME_GROUP::TYPE_PROGRAM_CONCEPT, "programme concept"},
		{SPI_PROGRAMME_GROUP::TYPE_MAGAZINE, "magazine"},
		{SPI_PROGRAMME_GROUP::TYPE_PROGRAM_COMPILATION, "programme compilation"},
		{SPI_PROGRAMME_GROUP::TYPE_OTHER_COLLECTION, "collection"},
		{SPI_PROGRAMME_GROUP::TYPE_OTHER_CHOICE, "choice"},
		{SPI_PROGRAMME_GROUP::TYPE_TOPIC, "topic"},
	};

	last_gi = info;

	popup->hide();
	ClearGrid(groups_grid);
	groups_stack->setCurrentIndex(info.groups.empty() ? 0 : 1);
	int i = 0;
	for(const SPI_PROGRAMME_GROUP& group : info.groups) {
		std::string name = BestName(group.long_names, group.medium_names, group.short_names);
		if(name.empty())
			name = group.id.empty() ? "(unnamed group)" : group.id;

		std::string subtitle;
		std::map<int,std::string>::const_iterator type_it = type_names.find(group.type);
		if(type_it != type_names.end())
			subtitle = type_it->second;
		if(group.num_of_items >= 0)
			subtitle += (subtitle.empty() ? "" : ", ") + std::to_string(group.num_of_items) + " items";

		groups_grid->addWidget(BuildCard(name, subtitle, false, group.logos, logo_data), i / cards_per_row, i % cards_per_row);
		i++;
	}

	if(xml_selector->currentIndex() == 2)
		RedrawXML();
}

void EDIScopeQtSPIWindow::Clear() {
	popup->hide();
	ClearGrid(services_grid);
	ClearGrid(groups_grid);
	last_si = SPI_SERVICE_INFORMATION();
	last_schedules.clear();
	last_gi = SPI_GROUP_INFORMATION();

	services_stack->setCurrentIndex(0);
	groups_stack->setCurrentIndex(0);
	RebuildProgrammeSelector();
	RedrawXML();
}
