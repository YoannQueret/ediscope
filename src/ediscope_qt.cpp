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

#include "ediscope_qt.h"

#include "ecc_countries.h"
#include "edi_network_source.h"
#include "edi_to_eti.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <fstream>
#include <sys/stat.h>

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QStatusBar>
#include <QStyleHints>
#include <QToolBar>
#include <QVBoxLayout>


// --- network connect history (persisted to $XDG_CONFIG_HOME/ediscope/network_history, or
// ~/.config/ediscope/network_history) ----------------------------------------------------------
static const size_t NETWORK_HISTORY_MAX_ENTRIES = 20;

// a sentinel entry at the top of the URI combo's dropdown - picking it opens a file chooser
// instead of being treated as a literal URI
static const QString NETWORK_URI_BROWSE_ITEM = "Open EDI/ETI File...";

static std::string GetConfigDir() {
	const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
	std::string base = (xdg_config_home && *xdg_config_home) ? xdg_config_home :
			std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config";
	return base + "/ediscope";
}

static std::string GetNetworkHistoryPath() {
	return GetConfigDir() + "/network_history";
}

static std::vector<std::string> LoadNetworkHistory() {
	static const std::string PREFIX_FILE = "file://";

	std::vector<std::string> result;
	std::ifstream file(GetNetworkHistoryPath());
	std::string line;
	while(std::getline(file, line) && result.size() < NETWORK_HISTORY_MAX_ENTRIES)
		// file:// sources aren't kept in history
		if(!line.empty() && line.compare(0, PREFIX_FILE.size(), PREFIX_FILE) != 0)
			result.push_back(line);
	return result;
}

static void SaveNetworkHistory(const std::vector<std::string>& history) {
	std::string config_dir = GetConfigDir();

	// best-effort directory creation (also covers a missing ~/.config); errors (incl. EEXIST) are ignored
	size_t pos = config_dir.find_last_of('/');
	if(pos != std::string::npos)
		mkdir(config_dir.substr(0, pos).c_str(), 0755);
	mkdir(config_dir.c_str(), 0755);

	std::ofstream file(GetNetworkHistoryPath());
	for(const std::string& uri : history)
		file << uri << "\n";
}

static int64_t SteadyNowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static QString ColoredText(const QString& text, const QColor& color) {
	return "<span style=\"color: " + color.name() + ";\">" + text + "</span>";
}

// best-effort, non-normative fallback for encoders that give a PI object neither a <scope> in its
// own content nor a MOT ScopeID/ScopeStart/ScopeEnd parameter (observed on a real BBC capture) -
// some legacy encoders still embed the SId/SCIdS directly in the ContentName instead, e.g.
// "w20251002dC22Ec0.EHB" (date 2025-10-02, SId 0xC22E, SCIdS 0) - ETSI TS 102 371 clause 6.4.5
// NOTE 2 explicitly warns ContentName carries no *guaranteed* meaning, so this is purely a last
// resort to still show *something* useful rather than silently dropping the schedule
static bool TryExtractSidFromLegacyContentName(const std::string& name, uint32_t& sid, int& scids) {
	for(size_t d_pos = name.find('d'); d_pos != std::string::npos; d_pos = name.find('d', d_pos + 1)) {
		size_t c_pos = d_pos + 5;
		if(c_pos >= name.size() || name[c_pos] != 'c')
			continue;

		std::string hex = name.substr(d_pos + 1, 4);
		if(hex.size() != 4 || !std::all_of(hex.begin(), hex.end(), [](unsigned char c) {return isxdigit(c);}))
			continue;

		size_t num_end = c_pos + 1;
		while(num_end < name.size() && isxdigit((unsigned char) name[num_end]))
			num_end++;
		if(num_end == c_pos + 1)
			continue;

		sid = (uint32_t) std::stoul(hex, nullptr, 16);
		scids = std::stoi(name.substr(c_pos + 1, num_end - c_pos - 1), nullptr, 16);
		return true;
	}

	return false;
}


// --- EDIScopeQt -----------------------------------------------------------------
EDIScopeQt::EDIScopeQt(EDIScopeQtOptions options) :
		options(options),
		ensemble_source(nullptr),
		engine_generation(0),
		last_frame_ms(0),
		discarded_fibs(0),
		first_frame_received(false),
		rec_file(nullptr),
		rec_duration_ms(0),
		rec_prebuffer_filled_ms(0),
		mux_rec_file(nullptr),
		mux_rec_frames(0),
		mux_rec_convert_to_eti(false),
		mux_rec_convert_errors(0),
		recording(REC_NONE),
		recording_start(0),
		dt_lto(FIC_ENSEMBLE::lto_none),
		current_audio_format_subchid(AUDIO_SERVICE::subchid_none),
		edi_timing_valid(false),
		spi_subchid(-1),
		spi_demux(nullptr),
		spi_mot_decoder(nullptr) {
	switch_service_label = options.initial_label;
	switch_service_sid = options.initial_sid;
	switch_service_scids = options.initial_scids;

	audio_output_type = AudioOutputType::SDL;
#ifndef EDISCOPE_DISABLE_SDL
	if(options.pcm_output)
#endif
		audio_output_type = AudioOutputType::PCM;
	if(options.wav_output)
		audio_output_type = AudioOutputType::WAV;
	if(options.untouched_output)
		audio_output_type = AudioOutputType::Untouched;

	if(this->options.source_format == EnsembleSource::FORMAT_ETI)
		ensemble_player = new ETIPlayer(audio_output_type, options.disable_int_catch_up, this);
	else
		ensemble_player = new EDIPlayer(audio_output_type, options.disable_int_catch_up, this);

	fic_decoder = new FICDecoder(this, options.disable_dyn_fic_msgs);
	pad_decoder = new PADDecoder(this, options.loose);
	mnsc_decoder = new MNSCDecoder(this);

	setWindowTitle("EDIScope v" + QString(EDISCOPE_VERSION));

	InitMenus();
	InitWidgets();

	// the "System" theme follows the desktop's colour scheme as it changes
	connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this]() {
		if(EDIScopeQtTheme::Current() == EDIScopeQtTheme::Theme::SYSTEM)
			ApplyTheme(EDIScopeQtTheme::Theme::SYSTEM);
	});

	// keyboard shortcuts work from anywhere in the window, except while typing a URI
	qApp->installEventFilter(this);

	connect(&status_timer, &QTimer::timeout, this, &EDIScopeQt::UpdateConnectionStatus);
	connect(&status_timer, &QTimer::timeout, this, &EDIScopeQt::UpdateEnsembleRecordingStatus);
	status_timer.start(500);
	UpdateConnectionStatus();

	// initial source from the command line: a file (or file:// URI), or stdin when something is
	// piped in - not an interactive terminal, which would otherwise be read as an ensemble
	if(!options.filename.empty() || !isatty(fileno(stdin))) {
		EnsembleSource *source;
		if(this->options.source_format == EnsembleSource::FORMAT_ETI)
			source = new ETISource(options.filename, this);
		else
			source = new EDISource(options.filename, this);
		// a file (given, or redirected to stdin) is played at the fixed 24 ms pace, regardless of its
		// timestamps - a pipe (e.g. from socat) is a live source
		struct stat stdin_stat;
		bool stdin_is_file = fstat(fileno(stdin), &stdin_stat) == 0 && S_ISREG(stdin_stat.st_mode);
		ensemble_player->SetIgnoreTimestamps(!options.filename.empty() || stdin_is_file);
		StartSource(source, QString::fromStdString(options.filename.empty() ? "stdin" : options.filename));
		if(this->options.source_format == EnsembleSource::FORMAT_EDI)
			ShowEnsembleWaitingMessage();
	}
}

EDIScopeQt::~EDIScopeQt() {
	qApp->removeEventFilter(this);

	// the tool windows have no Qt parent (so that they can go below this one): deleted here
	delete links_window;
	delete rdns_window;
	delete spi_window;
	delete fig_window;
	delete ann_window;
	delete mnsc_window;

	// signal every worker thread to stop up front, before blocking on any single one of them
	if(ensemble_source)
		ensemble_source->DoExit();
	if(ensemble_player)
		ensemble_player->PrepareStop();

	if(ensemble_source) {
		ensemble_source_thread.join();
		delete ensemble_source;
	}

	delete ensemble_player;

	// the player (which fed it) is gone already
	delete spi_demux;
	delete spi_mot_decoder;

	delete pad_decoder;
	delete fic_decoder;
	delete mnsc_decoder;

	// a recording still in progress (e.g. interrupted by a signal) is completed rather than truncated
	if(rec_file)
		fclose(rec_file);
}


// --- widgets -----------------------------------------------------------------
void EDIScopeQt::InitMenus() {
	QMenu *menu_file = menuBar()->addMenu("&File");
	QAction *action_open = menu_file->addAction("&Open EDI/ETI File...", this, &EDIScopeQt::OpenFile);
	action_open->setShortcut(QKeySequence::Open);
	menu_file->addSeparator();
	QAction *action_quit = menu_file->addAction("&Quit", this, &QWidget::close);
	action_quit->setShortcut(QKeySequence::Quit);

	QMenu *menu_tools = menuBar()->addMenu("&Tools");
	action_links = menu_tools->addAction("&Links");
	action_links->setCheckable(true);
	action_rdns = menu_tools->addAction("&RadioDNS");
	action_rdns->setCheckable(true);
	action_spi = menu_tools->addAction("&SPI / EPG");
	action_spi->setCheckable(true);
	action_ann = menu_tools->addAction("&Announcements");
	action_ann->setCheckable(true);
	action_mnsc = menu_tools->addAction("&MNSC");
	action_mnsc->setCheckable(true);
	action_fig = menu_tools->addAction("&FIG Analysis");
	action_fig->setCheckable(true);

	QMenu *menu_record = menuBar()->addMenu("&Record");
	action_record_service = menu_record->addAction("Record current &service", this, &EDIScopeQt::StartServiceRecording);
	action_record_service->setShortcut(QKeySequence(Qt::Key_R));
	action_record_service->setShortcutContext(Qt::WidgetShortcut);	// R itself is handled in HandleKey (not while typing a URI)
	action_record_service->setToolTip("The selected service's audio, as broadcast (untouched AAC/MP2 stream)");
	action_record_edi = menu_record->addAction("Record ensemble as &EDI (.edi)", this, [this]() {StartEnsembleRecording(false);});
	action_record_edi->setToolTip("Every EDI AF packet as received - all services, FIC and data (EDI sources only)");
	action_record_eti = menu_record->addAction("Record ensemble as E&TI (.eti)", this, [this]() {StartEnsembleRecording(true);});
	action_record_eti->setToolTip("Every frame as ETI-NI (converted from EDI, or as received from an ETI source) - all services, FIC and data; EDIScope can play it back");
	menu_record->addSeparator();
	action_stop_recording = menu_record->addAction("S&top recording", this, &EDIScopeQt::StopRecording);
	menu_record->setToolTipsVisible(true);

	// no sub-menu: opens the settings window right away
	menuBar()->addAction("&Settings", this, &EDIScopeQt::ShowSettings);

	QMenu *menu_help = menuBar()->addMenu("&Help");
	menu_help->addAction("&About EDIScope", this, &EDIScopeQt::ShowAbout);
}

QWidget* EDIScopeQt::InitToolBar() {
	QToolBar *toolbar = addToolBar("Main");
	toolbar->setMovable(false);
	toolbar->setFloatable(false);
	toolbar->setContextMenuPolicy(Qt::PreventContextMenu);

	combo_uri = new QComboBox(toolbar);
	combo_uri->setEditable(true);
	combo_uri->setInsertPolicy(QComboBox::NoInsert);
	combo_uri->setMaxVisibleItems(NETWORK_HISTORY_MAX_ENTRIES + 1);	// the whole history, plus the file picker entry
	combo_uri->setMinimumWidth(360);
	combo_uri->lineEdit()->setPlaceholderText("tcp://host:port");
	combo_uri->lineEdit()->setFont(EDIScopeQtTheme::MonoFont());
	combo_uri->setToolTip(
			"EDI network source\n"
			"tcp://host:port\n"
			"udp://host:port (unicast)\n"
			"udp://mcast-ip:port (multicast)\n"
			"\n"
			"Local file source\n"
			"file:///path/to/file.eti (or .edi)\n"
			"...or pick \"" + NETWORK_URI_BROWSE_ITEM + "\" from the dropdown");
	combo_uri->addItem(NETWORK_URI_BROWSE_ITEM);
	for(const std::string& uri : LoadNetworkHistory())
		combo_uri->addItem(QString::fromStdString(uri));
	combo_uri->setCurrentIndex(-1);
	combo_uri->lineEdit()->clear();
	connect(combo_uri, QOverload<int>::of(&QComboBox::activated), this, &EDIScopeQt::OnUriActivated);
	connect(combo_uri->lineEdit(), &QLineEdit::returnPressed, this, [this]() {
		if(!btn_connect->isChecked())
			btn_connect->setChecked(true);
	});
	toolbar->addWidget(combo_uri);

	btn_connect = new QPushButton("Connect", toolbar);
	btn_connect->setObjectName("connectButton");
	btn_connect->setCheckable(true);
	connect(btn_connect, &QPushButton::toggled, this, &EDIScopeQt::OnConnectToggled);
	toolbar->addWidget(btn_connect);

	btn_record = new QPushButton("● Record", toolbar);
	btn_record->setObjectName("recordButton");
	connect(btn_record, &QPushButton::clicked, this, &EDIScopeQt::StopRecording);
	toolbar->addWidget(btn_record);

	label_record = new QLabel(toolbar);
	label_record->setFont(EDIScopeQtTheme::MonoFont());
	label_record->setMinimumWidth(70);
	toolbar->addWidget(label_record);

	toolbar->addSeparator();

	// shortcuts to the tool windows - which ones (3 at most) is set in the settings window
	btn_links = new QPushButton("Links", toolbar);
	btn_links->setCheckable(true);
	btn_links->setToolTip("Service linking, Frequency Information and OE Services (ETSI EN 300 401 clauses 8.1.15/8.1.8/8.1.10), if the ensemble carries any");
	connect(btn_links, &QPushButton::toggled, this, &EDIScopeQt::OnLinksToggled);
	connect(btn_links, &QPushButton::toggled, action_links, &QAction::setChecked);
	connect(action_links, &QAction::triggered, btn_links, &QPushButton::setChecked);
	shortcut_actions["links"] = toolbar->addWidget(btn_links);

	btn_rdns = new QPushButton("RadioDNS", toolbar);
	btn_rdns->setCheckable(true);
	btn_rdns->setToolTip("RadioDNS (ETSI TS 103 270) resolution for the ensemble's services - trace where it works/blocks");
	connect(btn_rdns, &QPushButton::toggled, this, &EDIScopeQt::OnRDNSToggled);
	connect(btn_rdns, &QPushButton::toggled, action_rdns, &QAction::setChecked);
	connect(action_rdns, &QAction::triggered, btn_rdns, &QPushButton::setChecked);
	shortcut_actions["rdns"] = toolbar->addWidget(btn_rdns);

	btn_spi = new QPushButton("SPI / EPG", toolbar);
	btn_spi->setCheckable(true);
	btn_spi->setToolTip("Service and Programme Information (ETSI TS 102 371/102 818), if the ensemble carries any");
	connect(btn_spi, &QPushButton::toggled, this, &EDIScopeQt::OnSPIToggled);
	connect(btn_spi, &QPushButton::toggled, action_spi, &QAction::setChecked);
	connect(action_spi, &QAction::triggered, btn_spi, &QPushButton::setChecked);
	shortcut_actions["spi"] = toolbar->addWidget(btn_spi);

	btn_ann = new QPushButton("Announcements", toolbar);
	btn_ann->setCheckable(true);
	btn_ann->setToolTip("Announcements (ETSI EN 300 401 clause 8.1.6): clusters, support, history and consistency checks");
	connect(btn_ann, &QPushButton::toggled, this, &EDIScopeQt::OnAnnouncementsToggled);
	connect(btn_ann, &QPushButton::toggled, action_ann, &QAction::setChecked);
	connect(action_ann, &QAction::triggered, btn_ann, &QPushButton::setChecked);
	shortcut_actions["ann"] = toolbar->addWidget(btn_ann);

	btn_mnsc = new QPushButton("MNSC", toolbar);
	btn_mnsc->setCheckable(true);
	btn_mnsc->setToolTip("Transmitter set-up and control information carried in the MNSC (ETS 300 799): TII code and offset delay of each transmitter");
	connect(btn_mnsc, &QPushButton::toggled, this, &EDIScopeQt::OnMNSCToggled);
	connect(btn_mnsc, &QPushButton::toggled, action_mnsc, &QAction::setChecked);
	connect(action_mnsc, &QAction::triggered, btn_mnsc, &QPushButton::setChecked);
	shortcut_actions["mnsc"] = toolbar->addWidget(btn_mnsc);

	btn_fig = new QPushButton("FIG Analysis", toolbar);
	btn_fig->setCheckable(true);
	btn_fig->setToolTip("FIG repetition analysis against ETSI TS 103 176");
	connect(btn_fig, &QPushButton::toggled, this, &EDIScopeQt::OnFIGToggled);
	connect(btn_fig, &QPushButton::toggled, action_fig, &QAction::setChecked);
	connect(action_fig, &QAction::triggered, btn_fig, &QPushButton::setChecked);
	shortcut_actions["fig"] = toolbar->addWidget(btn_fig);
	ApplyToolbarShortcuts();

	QWidget *spacer = new QWidget(toolbar);
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	toolbar->addWidget(spacer);

	btn_mute = new QToolButton(toolbar);
	btn_mute->setText("Mute");
	btn_mute->setCheckable(true);
	btn_mute->setToolTip("Mute (M)");
	connect(btn_mute, &QToolButton::toggled, this, &EDIScopeQt::OnMuteToggled);
	toolbar->addWidget(btn_mute);

	slider_volume = new QSlider(Qt::Horizontal, toolbar);
	slider_volume->setRange(0, 100);
	slider_volume->setValue(100);
	slider_volume->setFixedWidth(110);
	slider_volume->setToolTip("Volume");
	slider_volume->setEnabled(ensemble_player->HasAudioVolumeControl());
	connect(slider_volume, &QSlider::valueChanged, this, &EDIScopeQt::OnVolumeChanged);
	toolbar->addWidget(slider_volume);

	return toolbar;
}

void EDIScopeQt::InitWidgets() {
	InitToolBar();

	QWidget *central = new QWidget(this);
	QVBoxLayout *central_layout = new QVBoxLayout(central);
	central_layout->setContentsMargins(12, 12, 12, 12);
	central_layout->setSpacing(12);

	// services table + CU bar
	panel_services = new EDIScopeQtPanel("Services", central);

	services_model = new EDIScopeQtServicesModel(this);
	services_model->SetTextProvider([this](const LISTED_SERVICE& s, int column) {return ServiceDetailCellText(s, column);});
	services_model->SetToolTipProvider([this](const LISTED_SERVICE& s) {return ServiceToolTip(s);});
	services_model->SetCompareProvider([this](const LISTED_SERVICE& a, const LISTED_SERVICE& b, int column) {return ServicesTableColumnCompare(a, b, column);});

	services_proxy = new EDIScopeQtServicesSortProxy(this);
	services_proxy->setSourceModel(services_model);
	services_proxy->setDynamicSortFilter(true);
	services_proxy->sort(EDIScopeQtServicesModel::COL_SLOT, Qt::AscendingOrder);

	services_view = new QTableView(panel_services);
	services_view->setModel(services_proxy);
	services_view->setSelectionBehavior(QAbstractItemView::SelectRows);
	services_view->setSelectionMode(QAbstractItemView::SingleSelection);
	services_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
	services_view->setAlternatingRowColors(true);
	services_view->setShowGrid(false);
	services_view->setWordWrap(false);
	services_view->setFont(EDIScopeQtTheme::MonoFont());
	services_view->verticalHeader()->hide();
	services_view->verticalHeader()->setDefaultSectionSize(24);
	services_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	services_view->setColumnHidden(EDIScopeQtServicesModel::COL_SLOT, true);

	QHeaderView *header = services_view->horizontalHeader();
	header->setHighlightSections(false);
	header->setSectionsClickable(true);
	header->setStretchLastSection(false);
	header->setSortIndicatorShown(false);
	static const int column_widths[] = {70, 60, 50, 160, 100, 110, 90, 90, 90, 90, 70, 90, 170, 64, 64, 56};
	for(int i = 0; i < EDIScopeQtServicesModel::COL_SLOT; i++) {
		header->setSectionResizeMode(i, EDIScopeQtServicesModel::IsCheckmarkColumn(i) ? QHeaderView::Fixed : QHeaderView::Interactive);
		header->resizeSection(i, column_widths[i]);
	}
	connect(header, &QHeaderView::sectionClicked, this, &EDIScopeQt::OnServicesHeaderClicked);

	// room for 10 services at least; a taller window shows more, beyond that they scroll
	services_view->setMinimumHeight(header->sizeHint().height() + 10 * services_view->verticalHeader()->defaultSectionSize() + 4);
	connect(services_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() {
		LISTED_SERVICE service;	// default: none
		GetSelectedService(service);
		SetService(service);
		cu_bar->SetSelectedSubchId(service.audio_service.subchid);
	});
	panel_services->BodyLayout()->addWidget(services_view, 1);

	QWidget *cu_box = new QWidget(panel_services);
	QVBoxLayout *cu_layout = new QVBoxLayout(cu_box);
	cu_layout->setContentsMargins(12, 9, 12, 9);
	cu_layout->setSpacing(5);
	cu_bar = new EDIScopeQtCUBar(cu_box);
	cu_bar->SetLabelProvider([this](const LISTED_SERVICE& s) {return ServiceDetailCellText(s, EDIScopeQtServicesModel::COL_LABEL);});
	connect(cu_bar, &EDIScopeQtCUBar::ServiceClicked, this, [this](int sid, int scids) {
		int row = services_model->Find(sid, scids);
		if(row != -1)
			SelectSourceRow(row);
	});
	label_cu_usage = new QLabel(cu_box);
	label_cu_usage->setFont(EDIScopeQtTheme::MonoFont());
	EDIScopeQtTheme::SetRole(label_cu_usage, "muted");
	cu_layout->addWidget(cu_bar);
	cu_layout->addWidget(label_cu_usage);
	cu_box->setObjectName("cuBox");
	cu_box->setAttribute(Qt::WA_StyledBackground);
	panel_services->BodyLayout()->addWidget(cu_box);

	central_layout->addWidget(panel_services, 1);

	// lower rows, on a grid so Dynamic Label's right edge lines up with Spectrum's right edge:
	//   Dynamic Label (Stereo Meter + Correlation + Spectrum width) | DL Plus (Level + Slideshow width)
	//   Stereo Meter | Correlation | Spectrum | Level | Slideshow
	QGridLayout *lower_layout = new QGridLayout();
	lower_layout->setHorizontalSpacing(12);
	lower_layout->setVerticalSpacing(12);

	panel_dl = new EDIScopeQtPanel("Dynamic Label", central);
	label_dl = new QLabel(panel_dl);
	label_dl->setTextFormat(Qt::PlainText);
	label_dl->setWordWrap(true);
	label_dl->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	label_dl->setTextInteractionFlags(Qt::TextSelectableByMouse);	// text selection + "Copy" context menu
	label_dl->setContentsMargins(12, 10, 12, 10);
	label_dl->setMinimumHeight(56);
	QFont dl_font = label_dl->font();
	dl_font.setPointSizeF(dl_font.pointSizeF() * 1.15);
	label_dl->setFont(dl_font);
	panel_dl->BodyLayout()->addWidget(label_dl);
	panel_dl->setEnabled(false);
	lower_layout->addWidget(panel_dl, 0, 0, 1, 3);

	panel_dl_plus = new EDIScopeQtPanel("Dynamic Label Plus", central);
	dl_plus = new EDIScopeQtDLPlusTable(panel_dl_plus);
	panel_dl_plus->BodyLayout()->addWidget(dl_plus);
	lower_layout->addWidget(panel_dl_plus, 0, 3, 1, 2);

	auto add_meter = [&](const QString& title, QWidget *meter, int column) {
		EDIScopeQtPanel *panel = new EDIScopeQtPanel(title, central);
		panel->BodyLayout()->setContentsMargins(0, 0, 0, 9);	// keeps clear of the panel's rounded bottom corners
		// a fixed-width meter is centered in its panel (which can be wider, e.g. for its title);
		// Spectrum takes the whole width
		bool fixed_width = meter->sizePolicy().horizontalPolicy() == QSizePolicy::Fixed;
		panel->BodyLayout()->addWidget(meter, 1, fixed_width ? Qt::AlignHCenter : Qt::Alignment());
		lower_layout->addWidget(panel, 1, column);
	};
	stereo_meter = new EDIScopeQtStereoMeter();
	correlation_meter = new EDIScopeQtCorrelationMeter();
	spectrum = new EDIScopeQtSpectrum();
	level_meter = new EDIScopeQtLevelMeter();
	add_meter("Stereo Meter", stereo_meter, 0);
	add_meter("Correlation", correlation_meter, 1);
	add_meter("Spectrum", spectrum, 2);
	add_meter("Level", level_meter, 3);

	EDIScopeQtPanel *panel_slideshow = new EDIScopeQtPanel("Slideshow", central);
	slideshow = new EDIScopeQtSlideshow(panel_slideshow);
	panel_slideshow->BodyLayout()->addWidget(slideshow);
	panel_slideshow->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
	lower_layout->addWidget(panel_slideshow, 1, 4);

	// only Spectrum (and so Dynamic Label above it) takes extra width
	lower_layout->setColumnStretch(2, 1);

	central_layout->addLayout(lower_layout, 0);

	setCentralWidget(central);

	links_window = new EDIScopeQtLinksWindow(this);
	connect(links_window, &EDIScopeQtLinksWindow::Closed, this, [this]() {btn_links->setChecked(false);});

	rdns_window = new EDIScopeQtRDNSWindow(this);
	connect(rdns_window, &EDIScopeQtRDNSWindow::Closed, this, [this]() {btn_rdns->setChecked(false);});
	connect(rdns_window, &EDIScopeQtRDNSWindow::TestRequested, this, &EDIScopeQt::StartRDNSTest);

	fig_window = new EDIScopeQtFIGWindow(this, [this]() {return fig_analyzer.Snapshot();});
	connect(fig_window, &EDIScopeQtFIGWindow::Closed, this, [this]() {btn_fig->setChecked(false);});
	connect(fig_window, &EDIScopeQtFIGWindow::ResetRequested, this, [this]() {fig_analyzer.Reset();});

	spi_window = new EDIScopeQtSPIWindow(this);
	connect(spi_window, &EDIScopeQtSPIWindow::Closed, this, [this]() {btn_spi->setChecked(false);});

	mnsc_window = new EDIScopeQtMNSCWindow(this);
	connect(mnsc_window, &EDIScopeQtMNSCWindow::Closed, this, [this]() {btn_mnsc->setChecked(false);});

	ann_window = new EDIScopeQtAnnouncementsWindow(this);
	connect(ann_window, &EDIScopeQtAnnouncementsWindow::Closed, this, [this]() {btn_ann->setChecked(false);});
	connect(ann_window, &EDIScopeQtAnnouncementsWindow::FIGAnalysisRequested, this, [this]() {
		if(btn_fig->isChecked())
			fig_window->raise();
		else
			btn_fig->setChecked(true);
	});

	// status bar
	QFont mono = EDIScopeQtTheme::MonoFont();
	status_connection = new QLabel(this);
	status_ensemble = new QLabel(this);
	status_ensemble->setTextFormat(Qt::RichText);
	status_datetime = new QLabel(this);
	status_edi_delay = new QLabel(this);
	status_asu = new QLabel(this);
	status_asu->setTextFormat(Qt::RichText);
	status_fib = new QLabel(this);
	progress_position = new QProgressBar(this);
	progress_position->setRange(0, 1000);
	progress_position->setFixedWidth(260);
	progress_position->hide();	// invisible until progress updated

	for(QLabel *label : {status_connection, status_ensemble, status_datetime, status_edi_delay, status_asu, status_fib})
		label->setFont(mono);

	// fixed width, so that an announcement never shifts the status bar: wide enough for the
	// longest type name, e.g. "▶ Programme Information"; a click opens the Announcements window
	// (caught by the application-wide event filter)
	status_asu->setFixedWidth(QFontMetrics(mono).horizontalAdvance("▶ Programme Information") + 24);
	status_asu->setCursor(Qt::PointingHandCursor);

	statusBar()->setSizeGripEnabled(false);
	statusBar()->addWidget(status_connection);
	statusBar()->addWidget(status_ensemble);
	statusBar()->addWidget(status_datetime);
	statusBar()->addWidget(status_edi_delay);
	statusBar()->addWidget(status_asu);
	statusBar()->addPermanentWidget(progress_position);
	statusBar()->addPermanentWidget(status_fib);

	status_datetime->setToolTip("Local date/time (from the ensemble)");
	status_fib->setToolTip("FIBs discarded because of a wrong CRC since the source was (re)started");
	UpdateEnsembleDisplay();
	UpdateCUBar();

	resize(1500, 980);
}


// --- services table -----------------------------------------------------------------
std::string EDIScopeQt::ServiceDetailCellText(const LISTED_SERVICE& service, int column) const {
	std::string text;
	switch(column) {
	case EDIScopeQtServicesModel::COL_SUBCHID:
		text = std::to_string(service.subchid);
		break;
	case EDIScopeQtServicesModel::COL_SID:	// without the "0x" prefix - unknown for a plain data sub-channel
		if(service.sid != LISTED_SERVICE::sid_none)
			text = StringTools::SidToHex(service.sid);
		break;
	case EDIScopeQtServicesModel::COL_ECC:	// without the "0x" prefix, only shown if it differs from the ensemble's own (FIG 0/9 exception)
		if(service.ecc != LISTED_SERVICE::ecc_none)
			text = StringTools::IntToHex(service.ecc, 2).substr(2);
		break;
	case EDIScopeQtServicesModel::COL_LABEL: {
		if(!service.is_audio) {
			std::map<uint32_t,int>::const_iterator ua_it = service.sid != LISTED_SERVICE::sid_none ?
					known_data_service_ua_types.find((uint32_t) service.sid) : known_data_service_ua_types.end();
			text = ua_it != known_data_service_ua_types.end() ? "(" + FICDecoder::ConvertUATypeToString(ua_it->second) + ")" : "(data service)";
			break;
		}
		text = FICDecoder::ConvertLabelToUTF8(service.label, nullptr);
		if(service.multi_comps)
			text = (!service.IsPrimary() ? "» " : "") + text + (service.IsPrimary() ? " »" : "");
		break; }
	case EDIScopeQtServicesModel::COL_SHORT_LABEL: {
		std::string label = FICDecoder::ConvertLabelToUTF8(service.label, nullptr);
		text = FICDecoder::DeriveShortLabelUTF8(label, service.label.short_label_mask);
		break; }
	case EDIScopeQtServicesModel::COL_CUS:	// first-last CU used, then the number of CUs
		if(!service.subchannel.IsNone() && !service.subchannel.pl.empty())
			text = std::to_string(service.subchannel.start) + "-" + std::to_string(service.subchannel.start + service.subchannel.size - 1) +
					" (" + std::to_string(service.subchannel.size) + ")";
		break;
	case EDIScopeQtServicesModel::COL_PROTECTION:
		text = service.subchannel.pl;
		break;
	case EDIScopeQtServicesModel::COL_BITRATE:
		if(!service.subchannel.IsNone() && !service.subchannel.pl.empty())
			text = std::to_string(service.subchannel.bitrate) + " kBit/s";
		break;
	case EDIScopeQtServicesModel::COL_FORMAT:	// only known for the currently playing sub-channel, while it is being decoded
		if(service.is_audio && service.audio_service.subchid == current_audio_format_subchid)
			text = current_audio_format.codec;
		break;
	case EDIScopeQtServicesModel::COL_SAMPLERATE:	// ditto
		if(service.is_audio && service.audio_service.subchid == current_audio_format_subchid && current_audio_format.samplerate_khz)
			text = std::to_string(current_audio_format.samplerate_khz) + " kHz";
		break;
	case EDIScopeQtServicesModel::COL_AUDIO_MODE:	// ditto
		if(service.is_audio && service.audio_service.subchid == current_audio_format_subchid)
			text = current_audio_format.mode;
		break;
	case EDIScopeQtServicesModel::COL_LANGUAGE:
		if(service.subchannel.language != FIC_SUBCHANNEL::language_none)
			text = FICDecoder::ConvertLanguageToString(service.subchannel.language);
		break;
	case EDIScopeQtServicesModel::COL_PTY:
		if(ensemble.inter_table_id != FIC_ENSEMBLE::inter_table_id_none) {
			int pty = service.pty_dynamic != LISTED_SERVICE::pty_none ? service.pty_dynamic : service.pty_static;
			if(pty != LISTED_SERVICE::pty_none)
				text = FICDecoder::ConvertPTYToString(pty, ensemble.inter_table_id);
		}
		break;
	case EDIScopeQtServicesModel::COL_SPI_SI:	// SPI Service Information present for this SId
		if(service.sid != LISTED_SERVICE::sid_none) {
			for(const SPI_SERVICE& spi_service : spi_info.services) {
				if(spi_service.has_dab_bearer && spi_service.sid == (uint32_t) service.sid) {
					text = "\xE2\x9C\x93";	// U+2713 CHECK MARK
					break;
				}
			}
		}
		break;
	case EDIScopeQtServicesModel::COL_SPI_PI:	// at least one day's SPI Programme Information present for this SId
		if(service.sid != LISTED_SERVICE::sid_none) {
			for(const std::pair<const std::pair<uint32_t,std::string>,SPI_SCHEDULE>& kv : spi_schedules) {
				if(kv.first.first == (uint32_t) service.sid) {
					text = "\xE2\x9C\x93";
					break;
				}
			}
		}
		break;
	case EDIScopeQtServicesModel::COL_RDNS: {	// RadioDNS: a checkmark only when fully resolved
		if(service.sid != LISTED_SERVICE::sid_none) {
			int scids = service.IsPrimary() ? 0 : service.scids;
			std::map<std::pair<int,int>,RDNS_RESULT>::const_iterator it = rdns_results.find(std::make_pair(service.sid, scids));
			if(it != rdns_results.end() && it->second.FullyResolved())
				text = "\xE2\x9C\x93";
		}
		break; }
	}

	return text;
}

std::string EDIScopeQt::ServiceToolTip(const LISTED_SERVICE& service) const {
	if(service.sid == LISTED_SERVICE::sid_none)
		return "";
	std::string text = "SID: " + StringTools::SidToHex(service.sid);

	int ecc = service.ecc != LISTED_SERVICE::ecc_none ? service.ecc : ensemble.ecc;
	if(ecc != FIC_ENSEMBLE::ecc_none) {
		text += "\n" "ECC: " + StringTools::IntToHex(ecc, 2).substr(2);
		// the ECC+country-nibble convention only applies to 16-bit (programme service) SIds
		if(service.sid <= 0xFFFF) {
			const char *country = LookupECCCountry(ecc, service.sid >> 12);
			if(country)
				text += " (" + std::string(country) + ")";
		}
	}
	if(!service.is_audio)
		text += "\n" "Data service (not playable)";
	return text;
}

int EDIScopeQt::ServicesTableColumnCompare(const LISTED_SERVICE& sa, const LISTED_SERVICE& sb, int column) const {
	switch(column) {
	case EDIScopeQtServicesModel::COL_SUBCHID:
		return sa.subchid - sb.subchid;
	case EDIScopeQtServicesModel::COL_SID:	// sid_none (-1) sorts before every real SId
		return sa.sid - sb.sid;
	case EDIScopeQtServicesModel::COL_ECC:	// ecc_none (-1) sorts before every real ECC
		return sa.ecc - sb.ecc;
	case EDIScopeQtServicesModel::COL_CUS: {	// by position (shown first), then number of CUs; unknown sorts last
		bool ka = !sa.subchannel.IsNone() && !sa.subchannel.pl.empty();
		bool kb = !sb.subchannel.IsNone() && !sb.subchannel.pl.empty();
		int va = ka ? sa.subchannel.start : INT_MAX;
		int vb = kb ? sb.subchannel.start : INT_MAX;
		if(va != vb)
			return va < vb ? -1 : 1;
		va = ka ? sa.subchannel.size : INT_MAX;
		vb = kb ? sb.subchannel.size : INT_MAX;
		return va < vb ? -1 : (va > vb ? 1 : 0); }
	case EDIScopeQtServicesModel::COL_BITRATE: {	// unknown sorts last
		int va = (!sa.subchannel.IsNone() && !sa.subchannel.pl.empty()) ? sa.subchannel.bitrate : INT_MAX;
		int vb = (!sb.subchannel.IsNone() && !sb.subchannel.pl.empty()) ? sb.subchannel.bitrate : INT_MAX;
		return va < vb ? -1 : (va > vb ? 1 : 0); }
	case EDIScopeQtServicesModel::COL_SAMPLERATE: {	// only known for the currently playing sub-channel - unknown sorts last
		int va = (sa.is_audio && sa.audio_service.subchid == current_audio_format_subchid && current_audio_format.samplerate_khz) ? (int) current_audio_format.samplerate_khz : INT_MAX;
		int vb = (sb.is_audio && sb.audio_service.subchid == current_audio_format_subchid && current_audio_format.samplerate_khz) ? (int) current_audio_format.samplerate_khz : INT_MAX;
		return va < vb ? -1 : (va > vb ? 1 : 0); }
	default:	// text-based columns - sort by the same text shown in the cell
		return ServiceDetailCellText(sa, column).compare(ServiceDetailCellText(sb, column));
	}
}

void EDIScopeQt::OnServicesHeaderClicked(int column) {
	// S/P/R are just presence checkmarks, not a value worth sorting by
	if(EDIScopeQtServicesModel::IsCheckmarkColumn(column))
		return;

	Qt::SortOrder order = Qt::AscendingOrder;
	if(services_proxy->sortColumn() == column && services_proxy->sortOrder() == Qt::AscendingOrder)
		order = Qt::DescendingOrder;

	services_proxy->sort(column, order);
	services_view->horizontalHeader()->setSortIndicatorShown(true);
	services_view->horizontalHeader()->setSortIndicator(column, order);
}

bool EDIScopeQt::GetSelectedService(LISTED_SERVICE& service) const {
	QModelIndexList rows = services_view->selectionModel()->selectedRows();
	if(rows.isEmpty())
		return false;
	QModelIndex source = services_proxy->mapToSource(rows.first());
	if(!source.isValid())
		return false;
	service = services_model->At(source.row());
	return true;
}

int EDIScopeQt::GetSelectedServiceIndex() const {
	QModelIndexList rows = services_view->selectionModel()->selectedRows();
	return rows.isEmpty() ? -1 : rows.first().row();
}

void EDIScopeQt::SelectSourceRow(int row) {
	QModelIndex index = services_proxy->mapFromSource(services_model->index(row, 0));
	if(!index.isValid())
		return;
	services_view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	services_view->scrollTo(index);
}

void EDIScopeQt::TrySwitchService(int index) {
	// switch, if allowed and index valid (in the order shown)
	if(!services_view->isEnabled() || index < 0 || index >= services_proxy->rowCount())
		return;

	QModelIndex proxy_index = services_proxy->index(index, 0);
	if(!(services_proxy->flags(proxy_index) & Qt::ItemIsSelectable))
		return;
	services_view->selectionModel()->setCurrentIndex(proxy_index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	services_view->scrollTo(proxy_index);
}

void EDIScopeQt::RebuildDataSubchannelRows() {
	services_model->RebuildDataSubchannelRows(known_subchannels, known_data_service_sids);
}

void EDIScopeQt::UpdateCUBar() {
	cu_bar->SetServices(services_model->Services());
	cu_bar->SetSubchannels(known_subchannels);

	LISTED_SERVICE selected;
	cu_bar->SetSelectedSubchId(GetSelectedService(selected) ? selected.audio_service.subchid : AUDIO_SERVICE::subchid_none);

	size_t used = cu_bar->UsedCUs();
	size_t free = used < EDIScopeQtCUBar::total_cus ? EDIScopeQtCUBar::total_cus - used : 0;
	label_cu_usage->setText(QString("CU usage: %1 / %2 (%3 free)").arg(used).arg(EDIScopeQtCUBar::total_cus).arg(free));

	size_t audio_services = 0;
	for(const LISTED_SERVICE& service : services_model->Services())
		if(service.is_audio)
			audio_services++;
	panel_services->SetInfo(audio_services == 1 ? "1 service" : QString("%1 services").arg(audio_services));
}


// --- service selection -----------------------------------------------------------------
void EDIScopeQt::SetService(const LISTED_SERVICE& service) {
	// (re)set title/record button
	if(!service.IsNone()) {
		setWindowTitle(QString::fromStdString(FICDecoder::ConvertLabelToUTF8(service.label, nullptr)) + " - EDIScope");
		UpdateRecordingUI();
	} else {
		setWindowTitle("EDIScope");
		UpdateRecordingUI();
	}

	UpdateAnnouncementSupport(service);

	// if the audio service changed, reset format/DL/slide + switch
	bool audio_service_changed = !ensemble_player->IsSameAudioService(service.audio_service);
	if(audio_service_changed) {
		// stop playback of old service
		if(options.rec_prebuffer_size_s > 0)
			ensemble_player->RemoveUntouchedStreamConsumer(this);
		ensemble_player->SetAudioService(AUDIO_SERVICE());

		pad_decoder->Reset();

		current_audio_format_subchid = AUDIO_SERVICE::subchid_none;
		services_model->RefreshAll();

		panel_dl->setEnabled(false);
		label_dl->clear();
		panel_dl->setToolTip(QString());
		dl_plus->ClearDLPlusInfo();
		level_meter->Reset();
		spectrum->Reset();
		stereo_meter->Reset();
		correlation_meter->Reset();

		if(!service.HasSLS())
			slideshow->ClearSlide();

		{
			std::lock_guard<std::mutex> lock(rec_mutex);

			// clear prebuffer, if enabled
			if(options.rec_prebuffer_size_s > 0) {
				rec_prebuffer.clear();
				rec_prebuffer_filled_ms = 0;
			}

			UpdateRecStatus(!service.IsNone());
		}

		// start playback of new service
		ensemble_player->SetAudioService(service.audio_service);
		if(options.rec_prebuffer_size_s > 0)
			ensemble_player->AddUntouchedStreamConsumer(this);
	}

	// (re)init SLS on new service (having SLS) only once
	if(service.HasSLS() && (audio_service_changed || slideshow->IsEmptySlide())) {
		slideshow->AwaitSlide();
		pad_decoder->SetMOTAppType(service.sls_app_type);
	}
}

void EDIScopeQt::UpdateAnnouncementSupport(const LISTED_SERVICE& service) {
	// a fixed-width indicator for the selected service (see InitWidgets): its number of supported
	// announcement types, or the announcement in progress - "▶" on this service's own sub-channel,
	// "◆" on another one. The details are in the tooltip and in the Announcements window
	QString click = "<br><br>Click to open the Announcements window.";
	if(service.IsNone()) {
		status_asu->setText(ColoredText("Ann. —", EDIScopeQtTheme::text_muted));
		status_asu->setToolTip("Announcements: no service selected" + click);
		return;
	}
	if(service.asu_flags == LISTED_SERVICE::asu_flags_none) {
		status_asu->setText(ColoredText("Ann. none", EDIScopeQtTheme::text_muted));
		status_asu->setToolTip("Announcements: this service cannot be interrupted by any announcement (no FIG 0/18 support)" + click);
		return;
	}

	// every supported type, and those in progress
	QStringList supported;
	std::vector<std::pair<QString, bool>> active;	// type name, on this service's own sub-channel
	for(int type = 0; type < 16; type++) {
		uint16_t type_flag = 1 << type;
		if(!(service.asu_flags & type_flag))
			continue;
		QString name = QString::fromStdString(FICDecoder::ConvertASuTypeToString(type));

		enum {NONE, OTHER, OWN} state = NONE;
		for(const cids_t::value_type& cid : service.cids) {
			asw_clusters_t::const_iterator it = ensemble.asw_clusters.find(cid);
			if(it == ensemble.asw_clusters.cend() || !it->second.IsActive() || !(it->second.asw_flags & type_flag))
				continue;
			if(it->second.subchid == service.audio_service.subchid)
				state = OWN;
			else if(state == NONE)
				state = OTHER;
		}

		if(state == NONE) {
			supported << name.toHtmlEscaped();
		} else {
			active.emplace_back(name, state == OWN);
			supported << "<b>" + QString(state == OWN ? "▶ " : "◆ ") + name.toHtmlEscaped() + "</b>";
		}
	}

	QString text;
	if(active.empty()) {
		text = QString("Ann. %1 type%2").arg(supported.size()).arg(supported.size() == 1 ? "" : "s");
	} else {
		bool own = false;
		for(const std::pair<QString, bool>& a : active)
			own |= a.second;
		QString label = active.size() == 1 ? active.front().first : QString("%1 in progress").arg(active.size());
		text = QString("<span style=\"background: %1; color: #0d1210;\">&nbsp;%2 %3&nbsp;</span>")
				.arg(own ? "#fbbf24" : "#22d3ee").arg(own ? "▶" : "◆").arg(label.toHtmlEscaped());
	}

	// tooltip: the supported types, the clusters
	QString tooltip = "<b>Announcements</b> that may interrupt this service (FIG 0/18):<br>" + supported.join("<br>");
	QString clusters;
	for(const cids_t::value_type& cid : service.cids) {
		asw_clusters_t::const_iterator it = ensemble.asw_clusters.find(cid);
		clusters += clusters.isEmpty() ? "" : ", ";
		clusters += QString::asprintf("0x%02X", cid);
		if(it != ensemble.asw_clusters.cend() && it->second.IsActive())
			clusters += " (in progress on SubChId " + QString::number(it->second.subchid) + ")";
	}
	tooltip += "<br><br>Cluster(s): " + (clusters.isEmpty() ? QString("none") : clusters);
	tooltip += "<br><br>▶ in progress on this service's sub-channel<br>◆ in progress on another sub-channel" + click;

	status_asu->setText(text);
	status_asu->setToolTip(tooltip);
}

void EDIScopeQt::UpdateEnsembleDisplay() {
	if(ensemble.IsNone()) {
		if(!status_ensemble->text().contains("Waiting"))
			status_ensemble->setText(ColoredText("Ensemble: -", EDIScopeQtTheme::text_muted));
		status_ensemble->setToolTip(QString());
		if(!panel_services->property("waiting").toBool())
			panel_services->SetTitle("Services");
		return;
	}
	panel_services->setProperty("waiting", false);

	std::string charset_name;
	std::string label_plain = FICDecoder::ConvertLabelToUTF8(ensemble.label, &charset_name);
	std::string short_label_plain = FICDecoder::DeriveShortLabelUTF8(label_plain, ensemble.label.short_label_mask);
	QString eid = QString::fromStdString(StringTools::IntToHex(ensemble.eid, 4).substr(2));

	std::string tooltip_text =
			"Short label: \"" + short_label_plain + "\"\n"
			"Label charset: " + charset_name + "\n"
			"EId: " + eid.toStdString() + "\n"
			"Alarm flag: " + (ensemble.al_flag ? "true" : "false");
	QString ecc_text;
	if(ensemble.ecc != FIC_ENSEMBLE::ecc_none) {
		ecc_text = QString::fromStdString(StringTools::IntToHex(ensemble.ecc, 2).substr(2));
		tooltip_text += "\n" "ECC: " + ecc_text.toStdString();
		const char *country = LookupECCCountry(ensemble.ecc, ensemble.eid >> 12);
		if(country) {
			tooltip_text += " (" + std::string(country) + ")";
			ecc_text += " (" + QString(country) + ")";
		}
	}
	if(ensemble.lto != FIC_ENSEMBLE::lto_none)
		tooltip_text += "\n" "LTO: " + FICDecoder::ConvertLTOToString(ensemble.lto);
	if(ensemble.inter_table_id != FIC_ENSEMBLE::inter_table_id_none)
		tooltip_text += "\n" "International table ID: " + StringTools::IntToHex(ensemble.inter_table_id, 2) + " (" + FICDecoder::ConvertInterTableIDToString(ensemble.inter_table_id) + ")";

	QString label = QString::fromStdString(label_plain).toHtmlEscaped();
	QString text = "Ensemble: " + ColoredText(label, EDIScopeQtTheme::text_strong) +
			" (" + QString::fromStdString(short_label_plain).toHtmlEscaped() + ")" +
			" · EId " + eid;
	if(!ecc_text.isEmpty())
		text += " · ECC " + ecc_text.toHtmlEscaped();

	status_ensemble->setText(text);
	status_ensemble->setToolTip(QString::fromStdString(tooltip_text));
	panel_services->SetTitle("Services — " + QString::fromStdString(label_plain) + " (EId " + eid + ")");
}

void EDIScopeQt::UpdateConnectionStatus() {
	// shape and text tell the state apart, the colour only reinforces it
	QString text;
	if(!ensemble_source) {
		text = ColoredText("○ Disconnected", EDIScopeQtTheme::text_muted);
	} else {
		int64_t last = last_frame_ms;
		if(last != 0 && SteadyNowMs() - last < 2000)
			text = ColoredText("● Receiving", EDIScopeQtTheme::ok);
		else
			text = ColoredText("▲ No data", EDIScopeQtTheme::warning);
	}
	status_connection->setText(text);
	status_connection->setToolTip(source_uri.isEmpty() ? QString("No source") : "Source: " + source_uri);

	unsigned int fibs = discarded_fibs;
	status_fib->setText(QString("FIB CRC errors: %1").arg(fibs));
	EDIScopeQtTheme::SetRole(status_fib, fibs ? "warning" : "");
}


// --- source handling -----------------------------------------------------------------
void EDIScopeQt::EnsureEnsemblePlayer(const std::string& format) {
	if(options.source_format == format)
		return;

	delete ensemble_player;

	options.source_format = format;
	if(format == EnsembleSource::FORMAT_ETI)
		ensemble_player = new ETIPlayer(audio_output_type, options.disable_int_catch_up, this);
	else
		ensemble_player = new EDIPlayer(audio_output_type, options.disable_int_catch_up, this);

	// preserve mute/volume across the swap
	slider_volume->setEnabled(ensemble_player->HasAudioVolumeControl());
	ensemble_player->SetAudioMute(btn_mute->isChecked());
	ensemble_player->SetAudioVolume(slider_volume->value() / 100.0);
}

void EDIScopeQt::ShowEnsembleWaitingMessage() {
	// an EDI source's frames are only actually decoded once their own embedded timestamp comes
	// due (see EDIPlayer) - with a mux configured for a large TIST offset, that can hold back the
	// first FIG 0/0 (ensemble label) for that many seconds after the connection itself succeeded
	status_ensemble->setText(ColoredText("<i>Waiting for timestamp synchronisation...</i>", EDIScopeQtTheme::text_muted));
	status_ensemble->setToolTip(
			"EDI frames are held back and only decoded once their own embedded timestamp comes due.\n"
			"With a mux configured for a large TIST offset, this can take several seconds - even "
			"though the connection itself already succeeded.");
	panel_services->SetTitle("Services — waiting for timestamp synchronisation...");
	panel_services->setProperty("waiting", true);
}

void EDIScopeQt::StartSource(EnsembleSource *source, const QString& uri) {
	ensemble_source = source;
	source_uri = uri;
	last_frame_ms = 0;
	discarded_fibs = 0;
	first_frame_received = false;
	// the FIG analysis only starts with its window - right away, if it is already open
	if(fig_window->isVisible())
		StartFIGAnalysis();
	ensemble_source_thread = std::thread(&EnsembleSource::Main, ensemble_source);
	UpdateConnectionStatus();
	UpdateRecordingUI();
}

void EDIScopeQt::StopEnsembleSource() {
	// a recording doesn't outlive its source
	StopRecording();

	StopSPIService();
	known_service_names.clear();

	if(ensemble_source) {
		ensemble_source->DoExit();
		ensemble_source_thread.join();
		delete ensemble_source;
		ensemble_source = nullptr;
	}
	source_uri.clear();
	history_pending_uri.clear();

	// frames of the previous connection still waiting for their due time must not refill the FIC
	// (nor play their audio) after the reset below, and reports already on their way to the UI
	// thread are dropped (see PostFromEngine)
	ensemble_player->Flush();
	fic_decoder->Reset();
	mnsc_decoder->Reset();
	// a new source starts without analysis (see StartSource)
	fig_analysis_running = false;
	fig_analyzer.Reset();
	engine_generation++;

	services_view->selectionModel()->clearSelection();
	services_model->Clear();
	rdns_results.clear();
	rdns_window->Clear();
	rdns_tested_params.clear();
	rdns_pending.clear();
	service_linking_sets.clear();
	frequency_infos.clear();
	oe_services.clear();
	links_window->Clear();
	oe_asw_clusters.clear();
	ann_window->Clear();
	mnsc_window->Clear();
	known_subchannels.clear();
	known_data_service_sids.clear();
	known_data_service_ua_types.clear();
	ensemble = FIC_ENSEMBLE();
	panel_services->setProperty("waiting", false);
	UpdateEnsembleDisplay();
	UpdateAlarm();
	UpdateCUBar();
	ensemble_player->StopAudio();

	// position progress only applies to a file source
	progress_position->hide();

	status_datetime->clear();
	status_datetime->setToolTip("Local date/time (from the ensemble)");
	status_edi_delay->clear();
	edi_timing_valid = false;

	utc_dt_curr = FIC_DAB_DT();
	dt_lto = FIC_ENSEMBLE::lto_none;
	dt_str_prev = "";

	last_frame_ms = 0;
	discarded_fibs = 0;
	UpdateConnectionStatus();
	UpdateRecordingUI();
}

void EDIScopeQt::AddToNetworkHistory(const std::string& uri) {
	std::vector<std::string> history = LoadNetworkHistory();
	history.erase(std::remove(history.begin(), history.end(), uri), history.end());
	history.insert(history.begin(), uri);
	if(history.size() > NETWORK_HISTORY_MAX_ENTRIES)
		history.resize(NETWORK_HISTORY_MAX_ENTRIES);
	SaveNetworkHistory(history);

	combo_uri->blockSignals(true);
	combo_uri->clear();
	combo_uri->addItem(NETWORK_URI_BROWSE_ITEM);
	for(const std::string& h : history)
		combo_uri->addItem(QString::fromStdString(h));
	combo_uri->setCurrentIndex(1);
	combo_uri->blockSignals(false);
}

void EDIScopeQt::OnUriActivated(int index) {
	if(combo_uri->itemText(index) != NETWORK_URI_BROWSE_ITEM)
		return;
	combo_uri->lineEdit()->clear();
	OpenFile();
}

void EDIScopeQt::OpenFile() {
	QString filename = QFileDialog::getOpenFileName(this, "Open EDI/ETI File", QString(), "ETI/EDI files (*.eti *.edi);;All files (*)");
	if(filename.isEmpty())
		return;

	combo_uri->lineEdit()->setText("file://" + filename);

	// same as pressing Enter in the URI box - start right away (restart, if already connected)
	if(btn_connect->isChecked())
		btn_connect->setChecked(false);
	btn_connect->setChecked(true);
}

void EDIScopeQt::OnConnectToggled(bool checked) {
	if(!checked) {
		StopEnsembleSource();
		combo_uri->setEnabled(true);
		btn_connect->setText("Connect");
		return;
	}

	std::string uri = combo_uri->currentText().trimmed().toStdString();

	auto fail = [this](const QString& message) {
		QMessageBox::critical(this, "Connection error", message);
		btn_connect->blockSignals(true);
		btn_connect->setChecked(false);
		btn_connect->blockSignals(false);
	};

	static const std::string PREFIX_FILE = "file://";
	if(uri.compare(0, PREFIX_FILE.size(), PREFIX_FILE) == 0) {
		std::string path = StringTools::FilePathFromArg(uri);

		// no way to pick a format in this box - infer it from the extension (defaulting to ETI)
		std::string lower_path = path;
		std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
		bool is_edi = lower_path.size() >= 4 && lower_path.compare(lower_path.size() - 4, 4, ".edi") == 0;

		StopEnsembleSource();
		EnsureEnsemblePlayer(is_edi ? EnsembleSource::FORMAT_EDI : EnsembleSource::FORMAT_ETI);
		if(is_edi)
			ShowEnsembleWaitingMessage();

		ensemble_player->SetIgnoreTimestamps(true);	// a file: fixed 24 ms pace (see EDIPlayer)
		if(is_edi)
			StartSource(new EDISource(path, this), QString::fromStdString(uri));
		else
			StartSource(new ETISource(path, this), QString::fromStdString(uri));

		combo_uri->setEnabled(false);
		btn_connect->setText("Disconnect");
		return;
	}

	static const std::string PREFIX_TCP = "tcp://";
	static const std::string PREFIX_UDP = "udp://";

	bool is_tcp;
	std::string rest;
	if(uri.compare(0, PREFIX_TCP.size(), PREFIX_TCP) == 0) {
		is_tcp = true;
		rest = uri.substr(PREFIX_TCP.size());
	} else if(uri.compare(0, PREFIX_UDP.size(), PREFIX_UDP) == 0) {
		is_tcp = false;
		rest = uri.substr(PREFIX_UDP.size());
	} else {
		fail("Unsupported URI - use tcp://host:port, udp://host:port or file:///path");
		return;
	}

	size_t colon_pos = rest.find_last_of(':');
	std::string host = colon_pos == std::string::npos ? "" : rest.substr(0, colon_pos);
	std::string port_str = colon_pos == std::string::npos ? "" : rest.substr(colon_pos + 1);

	int port = -1;
	if(!port_str.empty()) {
		try {
			size_t idx;
			port = std::stoi(port_str, &idx);
			if(idx != port_str.size())
				port = -1;
		} catch(const std::exception&) {
			port = -1;
		}
	}
	if(port <= 0 || port > 65535) {
		fail("Invalid or missing port in URI '" + QString::fromStdString(uri) + "'");
		return;
	}
	if(is_tcp && host.empty()) {
		fail("tcp:// requires a host");
		return;
	}

	// take over from whatever was running before (e.g. the initial command line source)
	StopEnsembleSource();
	EnsureEnsemblePlayer(EnsembleSource::FORMAT_EDI);
	ShowEnsembleWaitingMessage();

	ensemble_player->SetIgnoreTimestamps(false);
	history_pending_uri = uri;	// see EnsembleProcessFrame
	if(is_tcp)
		StartSource(new EDITCPSource(host, port, this), QString::fromStdString(uri));
	else
		StartSource(new EDIUDPSource(host, port, this), QString::fromStdString(uri));

	combo_uri->setEnabled(false);
	btn_connect->setText("Disconnect");
}


// --- recording, audio -----------------------------------------------------------------
QString EDIScopeQt::RecordingStartString(time_t start) {
	struct tm* start_tm = localtime(&start);
	char start_string[22] = "";
	if(start_tm)
		strftime(start_string, sizeof(start_string), "%F - %H-%M-%S", start_tm);
	else
		perror("EDIScopeQt: error while getting local time");
	return QString(start_string);
}

std::string EDIScopeQt::CleanFileName(std::string name) {
	// escape the forbidden '/' character
	std::replace(name.begin(), name.end(), '/', '_');
	return name;
}

void EDIScopeQt::StartServiceRecording() {
	LISTED_SERVICE service;
	if(recording != REC_NONE || !GetSelectedService(service))
		return;

	// start timestamp: the prebuffer's, if available
	time_t start;
	{
		std::lock_guard<std::mutex> lock(rec_mutex);
		start = !rec_prebuffer.empty() ? rec_prebuffer.front().ts : time(nullptr);
	}

	std::string label = FICDecoder::ConvertLabelToUTF8(service.label, nullptr);
	std::string extension = ensemble_player->GetUntouchedStreamFileExtension();
	std::string new_rec_filename = options.recordings_path + "/" + RecordingStartString(start).toStdString() + " - " + CleanFileName(label) + "." + extension;
	FILE* new_rec_file = fopen(new_rec_filename.c_str(), "wb");
	if(!new_rec_file) {
		int fopen_errno = errno;	// save before overwrite
		QMessageBox::critical(this, "EDIScope", "Could not start recording into\n" + QString::fromStdString(new_rec_filename) + "\n\n" + QString(strerror(fopen_errno)));
		return;
	}

	// no service switch while recording a service
	services_view->setEnabled(false);
	cu_bar->setEnabled(false);

	fprintf(stderr, "EDIScopeQt: service recording started into '%s'\n", new_rec_filename.c_str());

	{
		std::lock_guard<std::mutex> lock(rec_mutex);

		rec_file = new_rec_file;
		rec_filename = new_rec_filename;

		// write prebuffer
		for(const EDIScopeQtRecSample& s : rec_prebuffer)
			if(fwrite(&s.data[0], s.data.size(), 1, rec_file) != 1)
				perror("EDIScopeQt: error while writing untouched stream prebuffer to file");
		rec_duration_ms = rec_prebuffer_filled_ms;

		// clear prebuffer
		rec_prebuffer.clear();
		rec_prebuffer_filled_ms = 0;
	}

	if(options.rec_prebuffer_size_s == 0)
		ensemble_player->AddUntouchedStreamConsumer(this);

	recording = REC_SERVICE;
	recording_start = start;
	recording_type = "Service \"" + QString::fromStdString(label) + "\" (SId " + QString::fromStdString(StringTools::SidToHex(service.sid)) +
			"), untouched audio stream (." + QString::fromStdString(extension) + ")";
	UpdateRecordingUI();
}

void EDIScopeQt::StartEnsembleRecording(bool as_eti) {
	if(recording != REC_NONE || !ensemble_source)
		return;

	bool source_edi = options.source_format == EnsembleSource::FORMAT_EDI;
	if(!as_eti && !source_edi)
		return;	// an ETI source can't be recorded as EDI

	time_t start = time(nullptr);
	std::string label = ensemble.IsNone() ? "ensemble" : FICDecoder::ConvertLabelToUTF8(ensemble.label, nullptr);
	std::string new_filename = options.recordings_path + "/" + RecordingStartString(start).toStdString() + " - " + CleanFileName(label) + (as_eti ? ".eti" : ".edi");
	FILE* new_file = fopen(new_filename.c_str(), "wb");
	if(!new_file) {
		int fopen_errno = errno;	// save before overwrite
		QMessageBox::critical(this, "EDIScope", "Could not start recording into\n" + QString::fromStdString(new_filename) + "\n\n" + QString(strerror(fopen_errno)));
		return;
	}

	fprintf(stderr, "EDIScopeQt: ensemble recording started into '%s'\n", new_filename.c_str());

	{
		std::lock_guard<std::mutex> lock(mux_rec_mutex);
		mux_rec_file = new_file;
		mux_rec_frames = 0;
		mux_rec_convert_to_eti = as_eti && source_edi;
		mux_rec_convert_errors = 0;
	}
	mux_rec_filename = new_filename;

	recording = REC_ENSEMBLE;
	recording_start = start;
	if(!as_eti)
		recording_type = "Full ensemble, EDI (AF packets as received, .edi)";
	else if(source_edi)
		recording_type = "Full ensemble, ETI-NI converted from EDI (.eti) - EDIScope can play it back";
	else
		recording_type = "Full ensemble, ETI-NI (frames as received, .eti) - EDIScope can play it back";
	UpdateRecordingUI();
}

void EDIScopeQt::StopRecording() {
	if(recording == REC_NONE)
		return;

	std::string filename;
	long int duration_ms = 0;

	if(recording == REC_SERVICE) {
		std::lock_guard<std::mutex> lock(rec_mutex);
		if(options.rec_prebuffer_size_s == 0)
			ensemble_player->RemoveUntouchedStreamConsumer(this);
		if(rec_file) {
			fclose(rec_file);
			rec_file = nullptr;
		}
		filename = rec_filename;
		duration_ms = rec_duration_ms;

		// service switch possible again
		services_view->setEnabled(true);
		cu_bar->setEnabled(true);
	} else {
		std::lock_guard<std::mutex> lock(mux_rec_mutex);
		if(mux_rec_file) {
			fclose(mux_rec_file);
			mux_rec_file = nullptr;
		}
		filename = mux_rec_filename;
		duration_ms = (long int) mux_rec_frames * 24;	// an ETI/EDI frame is 24 ms
	}
	fprintf(stderr, "EDIScopeQt: recording stopped\n");

	recording = REC_NONE;
	UpdateRecordingUI();
	{
		std::lock_guard<std::mutex> lock(rec_mutex);
		UpdateRecStatus(true);
	}

	if(mux_rec_convert_errors)
		recording_type += QString(" - %1 EDI packet(s) could not be converted").arg(mux_rec_convert_errors);
	ShowRecordingSummary(QString::fromStdString(filename), duration_ms);
}

void EDIScopeQt::ShowRecordingSummary(const QString& filename, long int duration_ms) {
	QFileInfo info(filename);
	QString rows;
	auto add_row = [&](const QString& name, const QString& value) {
		rows += "<tr><td style=\"color: " + EDIScopeQtTheme::text_muted.name() + "; padding-right: 14px;\">" + name + "</td><td>" + value + "</td></tr>";
	};
	add_row("File", info.fileName().toHtmlEscaped());
	add_row("Folder", info.absolutePath().toHtmlEscaped());
	add_row("Type", recording_type.toHtmlEscaped());
	add_row("Started", RecordingStartString(recording_start).replace(" - ", " ").replace(QRegularExpression("(\\d\\d)-(\\d\\d)-(\\d\\d)$"), "\\1:\\2:\\3"));
	add_row("Duration", QString::fromStdString(StringTools::MsToTimecode(duration_ms)));
	add_row("Size", QString::fromStdString(StringTools::FormatByteSize(info.exists() ? (size_t) info.size() : 0)));

	QMessageBox box(this);
	box.setWindowTitle("Recording finished");
	box.setIcon(QMessageBox::Information);
	box.setTextFormat(Qt::RichText);
	box.setText("<b>Recording finished</b><br><br><table>" + rows + "</table>");
	box.setTextInteractionFlags(Qt::TextSelectableByMouse);
	QPushButton *open_folder = box.addButton("Open folder", QMessageBox::ActionRole);
	box.addButton(QMessageBox::Close);
	box.exec();
	if(box.clickedButton() == open_folder)
		QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
}

void EDIScopeQt::UpdateRecordingUI() {
	LISTED_SERVICE service;
	bool idle = recording == REC_NONE;
	action_record_service->setEnabled(idle && GetSelectedService(service));
	action_record_edi->setEnabled(idle && ensemble_source != nullptr && options.source_format == EnsembleSource::FORMAT_EDI);
	action_record_eti->setEnabled(idle && ensemble_source != nullptr);
	action_stop_recording->setEnabled(!idle);

	// the toolbar button only ever stops a recording - one is started from the Record menu
	btn_record->setEnabled(!idle);
	btn_record->setText(idle ? "● Record" : "■ Stop recording");
	btn_record->setToolTip(idle ? "Start a recording from the Record menu" :
			"Stop recording (R)\n" + recording_type + "\n" + QString::fromStdString(recording == REC_SERVICE ? rec_filename : mux_rec_filename));
	btn_record->setProperty("recording", !idle);
	btn_record->style()->unpolish(btn_record);
	btn_record->style()->polish(btn_record);

	if(recording == REC_ENSEMBLE)
		UpdateEnsembleRecordingStatus();
}

void EDIScopeQt::UpdateEnsembleRecordingStatus() {
	if(recording != REC_ENSEMBLE)
		return;
	label_record->setText(QString::fromStdString(StringTools::MsToTimecode((long int) mux_rec_frames * 24)));
	EDIScopeQtTheme::SetRole(label_record, "error");
	label_record->setToolTip("File name: \"" + QString::fromStdString(mux_rec_filename) + "\"");
}

void EDIScopeQt::ShowSettings() {
	QDialog dialog(this);
	dialog.setWindowTitle("Settings");
	dialog.setMinimumWidth(520);
	QVBoxLayout *layout = new QVBoxLayout(&dialog);
	layout->setSpacing(12);

	// theme
	QLabel *theme_title = new QLabel("Theme", &dialog);
	theme_title->setStyleSheet("font-weight: 600;");
	layout->addWidget(theme_title);
	QComboBox *theme_combo = new QComboBox(&dialog);
	theme_combo->addItem("System (follows the desktop)", EDIScopeQtTheme::ThemeName(EDIScopeQtTheme::Theme::SYSTEM));
	theme_combo->addItem("Dark", EDIScopeQtTheme::ThemeName(EDIScopeQtTheme::Theme::DARK));
	theme_combo->addItem("Light", EDIScopeQtTheme::ThemeName(EDIScopeQtTheme::Theme::LIGHT));
	theme_combo->setCurrentIndex(theme_combo->findData(EDIScopeQtTheme::ThemeName(EDIScopeQtTheme::Current())));
	theme_combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	QHBoxLayout *theme_layout = new QHBoxLayout();
	theme_layout->addWidget(theme_combo);
	theme_layout->addStretch(1);
	layout->addLayout(theme_layout);

	// recordings folder
	QLabel *folder_title = new QLabel("Recordings folder", &dialog);
	folder_title->setStyleSheet("font-weight: 600; margin-top: 6px;");
	layout->addWidget(folder_title);
	QHBoxLayout *folder_layout = new QHBoxLayout();
	QLineEdit *folder_edit = new QLineEdit(QString::fromStdString(options.recordings_path), &dialog);
	folder_edit->setFont(EDIScopeQtTheme::MonoFont());
	QPushButton *browse = new QPushButton("Browse...", &dialog);
	connect(browse, &QPushButton::clicked, &dialog, [&dialog, folder_edit]() {
		QString folder = QFileDialog::getExistingDirectory(&dialog, "Recordings folder", folder_edit->text());
		if(!folder.isEmpty())
			folder_edit->setText(folder);
	});
	folder_layout->addWidget(folder_edit, 1);
	folder_layout->addWidget(browse);
	layout->addLayout(folder_layout);
	if(options.recordings_path_given) {
		QLabel *note = new QLabel("Given with -r on the command line for this run - a folder chosen here is saved for the next runs.", &dialog);
		note->setWordWrap(true);
		EDIScopeQtTheme::SetRole(note, "muted");
		layout->addWidget(note);
	}

	// toolbar shortcuts
	QLabel *shortcuts_title = new QLabel("Toolbar shortcuts (3 at most)", &dialog);
	shortcuts_title->setStyleSheet("font-weight: 600; margin-top: 6px;");
	layout->addWidget(shortcuts_title);
	QStringList current = ToolbarShortcuts();
	std::vector<std::pair<QString, QCheckBox*>> boxes;
	for(const std::pair<const char*, const char*>& tool : std::vector<std::pair<const char*, const char*>>{
			{"links", "Links"}, {"rdns", "RadioDNS"}, {"spi", "SPI / EPG"}, {"ann", "Announcements"}, {"mnsc", "MNSC"}, {"fig", "FIG Analysis"}}) {
		QCheckBox *box = new QCheckBox(tool.second, &dialog);
		box->setChecked(current.contains(tool.first));
		layout->addWidget(box);
		boxes.emplace_back(tool.first, box);
	}
	auto enforce_max = [boxes]() {
		int checked = 0;
		for(const auto& b : boxes)
			checked += b.second->isChecked();
		for(const auto& b : boxes)
			b.second->setEnabled(b.second->isChecked() || checked < 3);
	};
	for(const auto& b : boxes)
		connect(b.second, &QCheckBox::toggled, &dialog, enforce_max);
	enforce_max();

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addSpacing(6);
	layout->addWidget(buttons);

	if(dialog.exec() != QDialog::Accepted)
		return;

	QSettings settings;
	QString folder = folder_edit->text().trimmed();
	if(!folder.isEmpty() && folder.toStdString() != options.recordings_path) {
		options.recordings_path = folder.toStdString();
		settings.setValue("recordings/path", folder);
	}

	QStringList shortcuts;
	for(const auto& b : boxes)
		if(b.second->isChecked())
			shortcuts << b.first;
	settings.setValue("toolbar/shortcuts", shortcuts);
	ApplyToolbarShortcuts();

	EDIScopeQtTheme::Theme theme = EDIScopeQtTheme::ThemeFromName(theme_combo->currentData().toString());
	settings.setValue("appearance/theme", EDIScopeQtTheme::ThemeName(theme));
	if(theme != EDIScopeQtTheme::Current())
		ApplyTheme(theme);
}

void EDIScopeQt::ApplyTheme(EDIScopeQtTheme::Theme theme) {
	EDIScopeQtTheme::Apply(*qApp, theme);

	// what isn't coloured by the style sheet itself, nor read from the theme when painted:
	// texts and table cells given a colour when they were filled
	services_model->RefreshAll();
	UpdateConnectionStatus();
	UpdateEnsembleDisplay();
	LISTED_SERVICE service;
	GetSelectedService(service);
	UpdateAnnouncementSupport(service);
	dl_plus->Refresh();
	rdns_window->ApplyTheme();
	fig_window->ApplyTheme();
	ann_window->ApplyTheme();
	mnsc_window->ApplyTheme();
}

QStringList EDIScopeQt::ToolbarShortcuts() {
	QSettings settings;
	if(!settings.contains("toolbar/shortcuts"))
		return {"links", "rdns", "spi"};
	return settings.value("toolbar/shortcuts").toStringList().mid(0, 3);
}

void EDIScopeQt::ApplyToolbarShortcuts() {
	QStringList shortcuts = ToolbarShortcuts();
	for(const std::pair<const QString, QAction*>& kv : shortcut_actions)
		kv.second->setVisible(shortcuts.contains(kv.first));
}


// --- FIG analysis window -----------------------------------------------------------------
void EDIScopeQt::OnFIGToggled(bool checked) {
	if(!checked) {
		fig_window->hide();
		return;
	}
	fig_window->AlignToParent();
	if(!fig_analysis_running)
		StartFIGAnalysis();
	fig_window->show();
	fig_window->raise();
}

// --- announcements window -----------------------------------------------------------------
void EDIScopeQt::OnAnnouncementsToggled(bool checked) {
	if(!checked) {
		ann_window->hide();
		return;
	}
	ann_window->AlignToParent();
	ann_window->show();
	ann_window->raise();
}

// --- MNSC window -----------------------------------------------------------------
void EDIScopeQt::OnMNSCToggled(bool checked) {
	if(!checked) {
		mnsc_window->hide();
		return;
	}
	mnsc_window->AlignToParent();
	mnsc_window->show();
	mnsc_window->raise();
}

void EDIScopeQt::MNSCTSCMessage(const MNSC_TSC& tsc) {
	PostFromEngine([this, tsc]() {mnsc_window->AddTSC(tsc);});
}

void EDIScopeQt::MNSCCRCError() {
	PostFromEngine([this]() {mnsc_window->AddCRCError();});
}

void EDIScopeQt::UpdateAnnouncementServices() {
	std::vector<EDIScopeQtAnnouncementsWindow::SERVICE_ENTRY> list;
	for(const LISTED_SERVICE& service : services_model->Services()) {
		if(!service.is_audio || service.sid == LISTED_SERVICE::sid_none)
			continue;
		EDIScopeQtAnnouncementsWindow::SERVICE_ENTRY entry;
		entry.sid = service.sid;
		entry.primary = service.IsPrimary();
		entry.subchid = service.audio_service.subchid;
		entry.label = ServiceDetailCellText(service, EDIScopeQtServicesModel::COL_LABEL);
		entry.asu_flags = service.asu_flags;
		entry.cids = service.cids;
		list.push_back(entry);
	}
	ann_window->SetServices(list);
}

void EDIScopeQt::UpdateAnnouncementSubchannels() {
	std::set<int> subchids;
	for(const std::pair<const int, FIC_SUBCHANNEL>& kv : known_subchannels)
		subchids.insert(kv.first);
	ann_window->SetSubchannels(subchids);
}

void EDIScopeQt::UpdateAlarm() {
	// an alarm announcement in progress, in this ensemble (FIG 0/19, cluster 0xFF) or in another one
	// (FIG 0/26, OE cluster 0xFF) - only if alarm announcements are enabled (Al flag of FIG 0/0),
	// as receivers ignore them otherwise (ETSI EN 300 401 clause 8.1.6.0)
	QStringList where;
	if(ensemble.al_flag) {
		asw_clusters_t::const_iterator it = ensemble.asw_clusters.find(0xFF);
		if(it != ensemble.asw_clusters.cend() && it->second.IsActive() && (it->second.asw_flags & 0x0001)) {
			QString text = "SubChId " + QString::number(it->second.subchid);
			for(const LISTED_SERVICE& service : services_model->Services())
				if(service.is_audio && service.audio_service.subchid == it->second.subchid) {
					text += " (" + QString::fromStdString(ServiceDetailCellText(service, EDIScopeQtServicesModel::COL_LABEL)) + ")";
					break;
				}
			where << text;
		}
		for(const fic_oe_asw_clusters_t::value_type& kv : oe_asw_clusters)
			if(kv.second.oe_cid == 0xFF && kv.second.IsActive() && (kv.second.asw_flags & 0x0001))
				where << "other ensemble, EId " + QString::fromStdString(StringTools::IntToHex(kv.second.target_sid, 4).substr(2));
	}

	if(where.isEmpty())
		panel_services->SetAlert(QString());
	else
		panel_services->SetAlert("⚠ ALARM", "Alarm announcement in progress: " + where.join(", ") + "\nClick the Announcements indicator in the status bar for details");
}

void EDIScopeQt::StartFIGAnalysis() {
	fig_analyzer.Reset();
	fig_analysis_running = true;
}


void EDIScopeQt::ProcessUntouchedStream(const uint8_t* data, size_t len, size_t duration_ms) {
	std::lock_guard<std::mutex> lock(rec_mutex);

	bool update_status = false;

	// if recording in progress
	if(rec_file) {
		long int rec_duration_ms_old = rec_duration_ms;

		// write sample
		if(fwrite(data, len, 1, rec_file) != 1)
			perror("EDIScopeQt: error while writing untouched stream sample to file");
		rec_duration_ms += duration_ms;

		// update status only on seconds change
		update_status = rec_duration_ms / 1000 != rec_duration_ms_old / 1000;
	} else if(options.rec_prebuffer_size_s > 0) {
		long int rec_prebuffer_ms_old = rec_prebuffer_filled_ms;

		// append sample
		rec_prebuffer.emplace_back(data, len, duration_ms);
		rec_prebuffer_filled_ms += duration_ms;

		// remove samples while needed
		while(rec_prebuffer_filled_ms - (long int) rec_prebuffer.front().duration_ms >= (long int) options.rec_prebuffer_size_s * 1000) {
			rec_prebuffer_filled_ms -= rec_prebuffer.front().duration_ms;
			rec_prebuffer.pop_front();
		}

		// update status only on seconds change
		update_status = rec_prebuffer_filled_ms / 1000 != rec_prebuffer_ms_old / 1000;
	}

	if(update_status)
		PostFromEngine([this]() {
			std::lock_guard<std::mutex> lock(rec_mutex);
			UpdateRecStatus(true);
		});
}

void EDIScopeQt::UpdateRecStatus(bool decoding) {
	// if recording in progress
	if(rec_file) {
		label_record->setText(QString::fromStdString(StringTools::MsToTimecode(rec_duration_ms)));
		EDIScopeQtTheme::SetRole(label_record, "error");
		label_record->setToolTip("File name: \"" + QString::fromStdString(rec_filename) + "\"");
	} else if(options.rec_prebuffer_size_s > 0 && decoding) {
		// prebuffer enabled and currently decoding
		label_record->setText(QString::fromStdString(StringTools::MsToTimecode(rec_prebuffer_filled_ms)));
		EDIScopeQtTheme::SetRole(label_record, "muted");
		label_record->setToolTip("Prebuffer: " + QString::fromStdString(StringTools::MsToTimecode(options.rec_prebuffer_size_s * 1000)));
	} else {
		label_record->clear();
		label_record->setToolTip(QString());
	}
}

void EDIScopeQt::OnMuteToggled(bool checked) {
	ensemble_player->SetAudioMute(checked);
	btn_mute->setText(checked ? "Muted" : "Mute");
}

void EDIScopeQt::OnVolumeChanged(int value) {
	ensemble_player->SetAudioVolume(value / 100.0);

	// disable mute, if needed
	if(btn_mute->isChecked())
		btn_mute->setChecked(false);
}

void EDIScopeQt::ShowAbout() {
	QMessageBox::about(this, "About EDIScope",
			"<b>EDIScope " + QString(EDISCOPE_VERSION).toHtmlEscaped() + "</b><br>"
			"DAB+/EDI diagnostic player<br><br>"
			"Copyright (C) 2025-2026 Yoann Queret<br><br>"
			"This program is free software: you can redistribute it and/or modify it under the terms of the "
			"GNU General Public License as published by the Free Software Foundation, either version 3 of the "
			"License, or (at your option) any later version.<br><br>"
			"The included FEC library by KA9Q has a separate license (LGPL).");
}


// --- window events -----------------------------------------------------------------
void EDIScopeQt::closeEvent(QCloseEvent *event) {
	// prevent exit while recording
	if(recording != REC_NONE) {
		QMessageBox::critical(this, "EDIScope", "Cannot exit while recording!\nThe recording has to be stopped first.");
		event->ignore();
		return;
	}
	event->accept();

	// the Links/RadioDNS/SPI windows only ever hide themselves when closed (to keep their content) -
	// so the application must not wait for them: it quits with the main window. exit() rather than
	// quit(), which in Qt 6 first asks every window to close and gives up as soon as one refuses
	links_window->hide();
	rdns_window->hide();
	spi_window->hide();
	fig_window->hide();
	ann_window->hide();
	mnsc_window->hide();
	QMetaObject::invokeMethod(qApp, []() {QCoreApplication::exit(0);}, Qt::QueuedConnection);
}

bool EDIScopeQt::eventFilter(QObject *watched, QEvent *event) {
	if(watched == status_asu && event->type() == QEvent::MouseButtonRelease) {
		if(btn_ann->isChecked())
			ann_window->raise();
		else
			btn_ann->setChecked(true);
		return true;
	}
	if(event->type() == QEvent::KeyPress && isActiveWindow()) {
		// typing into a text field keeps its keys - incl. the URI box: an editable combo box has
		// the focus itself (not its line edit), and forwards the keys to it
		QWidget *focus = QApplication::focusWidget();
		QComboBox *combo = qobject_cast<QComboBox*>(focus);
		bool text_input = qobject_cast<QLineEdit*>(focus) || (combo && combo->isEditable());
		if(!text_input && HandleKey(static_cast<QKeyEvent*>(event)))
			return true;
	}
	return QMainWindow::eventFilter(watched, event);
}

bool EDIScopeQt::HandleKey(QKeyEvent *key_event) {
	Qt::KeyboardModifiers modifiers = key_event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
	int key = key_event->key();

	// 1 to 0 select the 1st to 10th service (11th to 20th with Alt), in the order shown
	int digit_index = -1;
	if(key >= Qt::Key_1 && key <= Qt::Key_9)
		digit_index = key - Qt::Key_1;
	else if(key == Qt::Key_0)
		digit_index = 9;

	// without Shift/Control/Alt/Meta (the keypad counts as no modifier)
	if(modifiers == Qt::NoModifier) {
		switch(key) {
		case Qt::Key_M:
			btn_mute->toggle();
			return true;
		case Qt::Key_R:
			// starts recording the current service, or stops whatever recording is in progress
			if(recording != REC_NONE)
				StopRecording();
			else
				StartServiceRecording();
			return true;
		case Qt::Key_Minus: {
			int index = GetSelectedServiceIndex();
			if(index != -1)
				TrySwitchService(index - 1);
			return true; }
		case Qt::Key_Plus: {
			int index = GetSelectedServiceIndex();
			if(index != -1)
				TrySwitchService(index + 1);
			return true; }
		}
		if(digit_index != -1) {
			TrySwitchService(digit_index);
			return true;
		}
	}

	// with Alt only
	if(modifiers == Qt::AltModifier && digit_index != -1) {
		TrySwitchService(digit_index + 10);
		return true;
	}

	// with Control only: copy DL to clipboard, if not empty
	if(modifiers == Qt::ControlModifier && key == Qt::Key_C) {
		if(!label_dl->hasSelectedText() && !label_dl->text().isEmpty()) {
			QApplication::clipboard()->setText(label_dl->text());
			return true;
		}
	}

	return false;
}


// --- engine reports (UI thread) -----------------------------------------------------------------
void EDIScopeQt::OnChangeFormat(const AUDIO_SERVICE_FORMAT& format) {
	current_audio_format = format;

	// remember which sub-channel this format applies to, so the services table can show it for that row only
	LISTED_SERVICE service;
	current_audio_format_subchid = GetSelectedService(service) ? service.audio_service.subchid : AUDIO_SERVICE::subchid_none;
	services_model->RefreshAll();
}

void EDIScopeQt::OnEDITiming(const EDI_TIMING& timing) {
	edi_timing_valid = timing.valid;
	if(timing.valid) {
		status_edi_delay->setText("Margin: " + QString::number((int) std::lround(timing.margin_delay_ms)) + " ms");
		status_edi_delay->setToolTip("Margin delay (how far ahead of schedule frames arrive)\n"
				"TIST@FCT0: " + QString::number((int) std::lround(timing.tist_at_fct0_ms)) + " ms");
	} else {
		status_edi_delay->clear();
		status_edi_delay->setToolTip(QString());
	}
}

void EDIScopeQt::OnChangeEnsemble(const FIC_ENSEMBLE& new_ensemble) {
	ensemble = new_ensemble;
	RescanRDNSServices();	// the ensemble's ECC (hence every service's GCC) may have just changed
	if(links_window->isVisible())
		links_window->SetEnsembleEid(ensemble.eid);
	if(rdns_window->isVisible())
		rdns_window->SetServices(BuildRDNSServiceList());	// the ECC/EId a manual test uses
	UpdateEnsembleDisplay();
	UpdateAlarm();
	ann_window->SetEnsemble(ensemble);
	services_model->RefreshAll();	// PTY names depend on the international table ID

	LISTED_SERVICE service;
	if(GetSelectedService(service))
		UpdateAnnouncementSupport(service);
}

void EDIScopeQt::OnChangeService(const LISTED_SERVICE& new_service) {
	std::string label = FICDecoder::ConvertLabelToUTF8(new_service.label, nullptr);
	if(!label.empty()) {
		known_service_names[(uint32_t) new_service.sid] = label;
		if(spi_window->isVisible())
			spi_window->SetKnownServiceNames(known_service_names);
	}

	bool add_new_row;
	int row = services_model->Upsert(new_service, &add_new_row);

	if(add_new_row) {
		// if first found service requested, adopt service params (for possible later changes)
		if(options.initial_first_found_service) {
			switch_service_sid = new_service.sid;
			switch_service_scids = new_service.scids;
			options.initial_first_found_service = false;
		}

		// set (initial) service
		if((!switch_service_label.empty() && label == switch_service_label) || (new_service.sid == switch_service_sid && new_service.scids == switch_service_scids))
			SelectSourceRow(row);
	} else {
		// set (updated) service
		LISTED_SERVICE selected;
		if(GetSelectedService(selected) && selected.sid == new_service.sid && selected.scids == new_service.scids)
			SetService(new_service);
	}

	RebuildDataSubchannelRows();
	UpdateCUBar();

	if(new_service.is_audio) {
		int test_scids = new_service.IsPrimary() ? 0 : new_service.scids;
		int test_ecc = new_service.ecc != LISTED_SERVICE::ecc_none ? new_service.ecc : ensemble.ecc;
		MaybeStartRDNSTest(new_service.sid, test_scids, test_ecc, ensemble.eid);
	}

	if(links_window->isVisible())
		links_window->SetServices(BuildLinksServiceList());
	if(rdns_window->isVisible())
		rdns_window->SetServices(BuildRDNSServiceList());
	UpdateAnnouncementServices();
}

void EDIScopeQt::OnRemoveService(const fic_service_id_t& id) {
	int row = services_model->Find(id.first, id.second);
	if(row == -1)
		return;

	// if the removed service is currently selected, stop playback and deselect it first
	LISTED_SERVICE selected;
	if(GetSelectedService(selected) && selected.sid == id.first && selected.scids == id.second)
		services_view->selectionModel()->clearSelection();

	services_model->Remove(row);

	RebuildDataSubchannelRows();
	UpdateCUBar();

	std::pair<int,int> key(id.first, id.second == LISTED_SERVICE::scids_none ? 0 : id.second);
	rdns_results.erase(key);
	rdns_tested_params.erase(key);

	if(links_window->isVisible())
		links_window->SetServices(BuildLinksServiceList());
	if(rdns_window->isVisible())
		rdns_window->SetServices(BuildRDNSServiceList());
	UpdateAnnouncementServices();
}

void EDIScopeQt::OnChangeSPIService(int subchid, int address, bool fec) {
	fprintf(stderr, "EDIScopeQt: SubChId %d: SPI service confirmed (packet address %d, FEC %s)\n",
			subchid, address, fec ? "applied" : "not applied");

	StopSPIService();

	spi_subchid = subchid;
	spi_mot_decoder = new MOTDirectoryDecoder(this);
	spi_demux = new MSCPacketDemux(address, spi_mot_decoder);
	ensemble_player->AddDataSink(spi_subchid, spi_demux);
	spi_window->SetSPIStatus(spi_subchid);
}

void EDIScopeQt::OnRemoveSPIService(int subchid) {
	fprintf(stderr, "EDIScopeQt: SubChId %d: SPI service removed\n", subchid);
	if(subchid == spi_subchid)
		StopSPIService();
}

void EDIScopeQt::StopSPIService() {
	if(spi_demux) {
		ensemble_player->RemoveDataSink(spi_subchid);
		delete spi_demux;
		spi_demux = nullptr;
		delete spi_mot_decoder;
		spi_mot_decoder = nullptr;
	}
	spi_subchid = -1;

	spi_info = SPI_SERVICE_INFORMATION();
	spi_logo_data.clear();
	spi_schedules.clear();
	spi_group_info = SPI_GROUP_INFORMATION();
	services_model->RefreshAll();	// clears the S/P columns' checkmarks too
	spi_window->Clear();
	spi_window->SetSPIStatus(-1);
}

void EDIScopeQt::OnChangeDynamicLabel(const DL_STATE& dl) {
	// consider clear display command
	if(dl.charset != -1) {
		std::string charset_name;
		std::string label = CharsetTools::ConvertTextToUTF8(&dl.raw[0], dl.raw.size(), dl.charset, false, &charset_name);

		// skip unsupported charsets
		if(!charset_name.empty()) {
			panel_dl->setEnabled(true);
			label_dl->setText(QString::fromStdString(label));
			panel_dl->setToolTip("Charset: " + QString::fromStdString(charset_name));
		}
	} else {
		panel_dl->setEnabled(true);
		label_dl->clear();
		panel_dl->setToolTip(QString());
	}

	// if present, forward DL Plus updates
	if(!dl.dl_plus_objects.empty())
		dl_plus->UpdateDLPlusInfo(dl);
}


// --- Links window -----------------------------------------------------------------
void EDIScopeQt::OnLinksToggled(bool checked) {
	if(!checked) {
		links_window->hide();
		return;
	}

	links_window->AlignToParent();
	links_window->SetEnsembleEid(ensemble.eid);
	// Service linking, Frequency Information and OE Services are decoded as soon as they're seen
	// on the FIC, independently of this window ever being open - replay whatever is already known
	for(const fic_service_linking_sets_t::value_type& kv : service_linking_sets)
		links_window->UpdateServiceLinking(kv.first, kv.second);
	for(const fic_frequency_infos_t::value_type& kv : frequency_infos)
		links_window->UpdateFrequencyInfo(kv.first, kv.second);
	for(const fic_oe_services_t::value_type& kv : oe_services)
		links_window->UpdateOEService(kv.first, kv.second);
	links_window->SetServices(BuildLinksServiceList());
	links_window->show();
	links_window->raise();
}

std::vector<EDIScopeQtLinksWindow::SERVICE_ENTRY> EDIScopeQt::BuildLinksServiceList() const {
	std::vector<EDIScopeQtLinksWindow::SERVICE_ENTRY> list;
	std::set<int> known_sids;

	for(const LISTED_SERVICE& service : services_model->Services()) {
		if(!service.is_audio || service.sid == LISTED_SERVICE::sid_none)
			continue;

		EDIScopeQtLinksWindow::SERVICE_ENTRY entry;
		entry.sid = service.sid;
		entry.scids = service.IsPrimary() ? 0 : service.scids;
		entry.label = ServiceDetailCellText(service, EDIScopeQtServicesModel::COL_LABEL);
		list.push_back(entry);
		known_sids.insert(service.sid);
	}

	// a FIG 0/6 linking set isn't necessarily only about services carried on this ensemble (its
	// key service and DAB_SID members can be elsewhere) - a placeholder entry for every such SId,
	// so its linking information can still be inspected
	for(const fic_service_linking_sets_t::value_type& kv : service_linking_sets) {
		const FIC_SERVICE_LINKING_SET& set = kv.second;
		if(set.is_data)
			continue;	// the Links window only navigates by (SId, SCIdS) as used for audio services

		std::vector<uint32_t> extra_sids;
		if(set.key_service_sid != FIC_SERVICE_LINKING_SET::sid_none)
			extra_sids.push_back(set.key_service_sid);
		for(const FIC_LINKED_SERVICE& member : set.members)
			if(member.type == FIC_LINKED_SERVICE::DAB_SID)
				extra_sids.push_back(member.id);

		for(uint32_t sid : extra_sids) {
			if(known_sids.count((int) sid))
				continue;
			known_sids.insert((int) sid);

			EDIScopeQtLinksWindow::SERVICE_ENTRY entry;
			entry.sid = (int) sid;
			entry.scids = 0;
			entry.label = "SId " + StringTools::SidToHex((int) sid) + " (not on this ensemble)";
			list.push_back(entry);
		}
	}

	return list;
}


// --- SPI window -----------------------------------------------------------------
void EDIScopeQt::OnSPIToggled(bool checked) {
	if(!checked) {
		spi_window->hide();
		return;
	}

	// everything is decoded as soon as it's seen, independently of this window ever being open -
	// replay whatever is already known
	spi_window->AlignToParent();
	spi_window->SetKnownServiceNames(known_service_names);
	spi_window->Update(spi_info, spi_logo_data);
	for(const std::pair<const std::pair<uint32_t,std::string>,SPI_SCHEDULE>& kv : spi_schedules)
		spi_window->UpdateSchedule(kv.second);
	spi_window->UpdateGroupInformation(spi_group_info, spi_logo_data);
	spi_window->show();
	spi_window->raise();
}


// --- RadioDNS -----------------------------------------------------------------
void EDIScopeQt::OnRDNSToggled(bool checked) {
	if(!checked) {
		rdns_window->hide();
		return;
	}

	rdns_window->AlignToParent();
	rdns_window->SetServices(BuildRDNSServiceList());
	// services are tested automatically as soon as they're known, independently of this window
	// ever being open - replay whatever was already found out about them in the meantime
	for(const std::pair<const std::pair<int,int>,RDNS_RESULT>& kv : rdns_results)
		rdns_window->UpdateResult(kv.first.first, kv.first.second, kv.second);
	rdns_window->show();
	rdns_window->raise();
}

std::vector<EDIScopeQtRDNSWindow::SERVICE_ENTRY> EDIScopeQt::BuildRDNSServiceList() const {
	std::vector<EDIScopeQtRDNSWindow::SERVICE_ENTRY> list;
	for(const LISTED_SERVICE& service : services_model->Services()) {
		if(!service.is_audio || service.sid == LISTED_SERVICE::sid_none)
			continue;

		EDIScopeQtRDNSWindow::SERVICE_ENTRY entry;
		entry.sid = service.sid;
		entry.scids = service.IsPrimary() ? 0 : service.scids;
		entry.ecc = service.ecc != LISTED_SERVICE::ecc_none ? service.ecc : ensemble.ecc;
		entry.eid = ensemble.eid;
		entry.label = ServiceDetailCellText(service, EDIScopeQtServicesModel::COL_LABEL);
		list.push_back(entry);
	}
	return list;
}

void EDIScopeQt::StartRDNSTest(int sid, int scids, int ecc, int eid) {
	std::pair<int,int> key(sid, scids);
	if(rdns_pending.find(key) != rdns_pending.end())
		return;
	rdns_pending.insert(key);
	rdns_tested_params[key] = std::make_pair(ecc, eid);

	// resolution itself (DNS + HTTP I/O) runs on a short-lived detached thread; its result is
	// handed back through the application object, which outlives this window - the window itself
	// may be gone by then, which the QPointer check (on the UI thread) catches
	QPointer<EDIScopeQt> self(this);
	unsigned int generation = engine_generation;
	std::thread([self, generation, sid, scids, ecc, eid]() {
		RDNS_RESULT result = RadioDNSResolver::Resolve(ecc, eid, (uint32_t) sid, false, scids);
		QMetaObject::invokeMethod(qApp, [self, generation, sid, scids, result]() {
			// the window is gone, or the source changed in the meantime - a stale result then
			if(!self || generation != self->engine_generation)
				return;
			std::pair<int,int> key(sid, scids);
			self->rdns_pending.erase(key);
			self->rdns_results[key] = result;
			self->services_model->RefreshAll();	// refreshes the R column's checkmark
			if(self->rdns_window->isVisible())
				self->rdns_window->UpdateResult(sid, scids, result);
		}, Qt::QueuedConnection);
	}).detach();
}

void EDIScopeQt::MaybeStartRDNSTest(int sid, int scids, int ecc, int eid) {
	if(ecc == FIC_ENSEMBLE::ecc_none || sid == LISTED_SERVICE::sid_none)
		return;

	std::pair<int,int> key(sid, scids);
	if(rdns_pending.find(key) != rdns_pending.end())
		return;

	// already (being) tested with these exact broadcast parameters
	std::map<std::pair<int,int>,std::pair<int,int>>::const_iterator it = rdns_tested_params.find(key);
	if(it != rdns_tested_params.end() && it->second == std::make_pair(ecc, eid))
		return;

	StartRDNSTest(sid, scids, ecc, eid);
}

void EDIScopeQt::RescanRDNSServices() {
	for(const EDIScopeQtRDNSWindow::SERVICE_ENTRY& entry : BuildRDNSServiceList())
		MaybeStartRDNSTest(entry.sid, entry.scids, entry.ecc, entry.eid);
}


// --- date/time -----------------------------------------------------------------
void EDIScopeQt::ShowDateTime(bool scheduled) {
	// if scheduled, update
	if(scheduled)
		utc_dt_curr = utc_dt_next;

	// if UTC date/time and LTO set, show, if output changed
	if(!utc_dt_curr.IsNone() && dt_lto != FIC_ENSEMBLE::lto_none) {
		std::string dt_str = FICDecoder::ConvertDateTimeToString(utc_dt_curr, dt_lto, false);
		if(dt_str_prev != dt_str) {
			dt_str_prev = dt_str;
			PostFromEngine([this, dt_str]() {
				status_datetime->setText("Local: " + QString::fromStdString(dt_str));
			});
		}
	}

	// if scheduled, (re)schedule
	if(scheduled) {
		if(!utc_dt_next.IsMsNone()) {
			// long form
			dt_update += std::chrono::milliseconds(1000 - utc_dt_next.ms);
			utc_dt_next.dt.tm_sec++;
			utc_dt_next.ms = 0;
		} else {
			// short form
			dt_update += std::chrono::minutes(1);
			utc_dt_next.dt.tm_min++;
		}
		if(mktime(&utc_dt_next.dt) == (time_t) -1)
			throw std::runtime_error("EDIScopeQt: error while normalizing date/time");
	}
}


// --- EnsembleSourceObserver (source thread) -----------------------------------------------------------------
void EDIScopeQt::EnsembleProcessFrame(const uint8_t *data) {
	last_frame_ms = SteadyNowMs();

	// full ensemble recording: every frame - an EDI AF packet as received (its length is in its
	// header) or converted to ETI-NI, or an ETI-NI frame as received (always 6144 bytes)
	{
		std::lock_guard<std::mutex> lock(mux_rec_mutex);
		if(mux_rec_file) {
			bool is_af = data[0] == 'A' && data[1] == 'F';
			if(is_af && mux_rec_convert_to_eti) {
				if(EDIToETI::Convert(data, mux_rec_eti_frame)) {
					if(fwrite(&mux_rec_eti_frame[0], mux_rec_eti_frame.size(), 1, mux_rec_file) != 1)
						perror("EDIScopeQt: error while writing ETI frame to file");
					mux_rec_frames++;
				} else {
					mux_rec_convert_errors++;
				}
			} else {
				size_t len = is_af ? 10 + (size_t) (data[2] << 24 | data[3] << 16 | data[4] << 8 | data[5]) + 2 : EDIToETI::eti_frame_size;
				if(fwrite(data, len, 1, mux_rec_file) != 1)
					perror("EDIScopeQt: error while writing ensemble frame to file");
				mux_rec_frames++;
			}
		}
	}

	// the source really works: a pending network URI can join the history now
	if(!first_frame_received.exchange(true))
		PostFromEngine([this]() {
			if(!history_pending_uri.empty()) {
				AddToNetworkHistory(history_pending_uri);
				history_pending_uri.clear();
			}
		});
	ensemble_player->ProcessFrame(data);
}

void EDIScopeQt::EnsembleUpdateProgress(const ENSEMBLE_PROGRESS& progress) {
	PostFromEngine([this, progress]() {
		progress_position->setValue((int) (progress.value * 1000));
		progress_position->setFormat(QString::fromStdString(progress.text));
		progress_position->setTextVisible(true);
		progress_position->show();
	});
}

void EDIScopeQt::EnsembleDoRegularWork() {
	// if UTC date/time set and scheduled time reached, update/show/reschedule
	if(!utc_dt_curr.IsNone() && std::chrono::steady_clock::now() >= dt_update)
		ShowDateTime(true);

	// detect services hot-removed from the ensemble (no longer listed in FIG 0/2)
	fic_decoder->CheckServiceTimeouts();
}


// --- EnsemblePlayerObserver (player threads) -----------------------------------------------------------------
void EDIScopeQt::EnsembleChangeFormat(const AUDIO_SERVICE_FORMAT& format) {
	PostFromEngine([this, format]() {OnChangeFormat(format);});
}

void EDIScopeQt::EnsembleAudioLevel(const std::vector<double>& channel_dbfs) {
	PostFromEngine([this, channel_dbfs]() {level_meter->SetLevels(channel_dbfs);});
}

void EDIScopeQt::EnsembleAudioSpectrum(const AUDIO_SPECTRUM& audio_spectrum) {
	PostFromEngine([this, audio_spectrum]() {spectrum->SetSpectrum(audio_spectrum);});
}

void EDIScopeQt::EnsembleAudioStereo(const AUDIO_STEREO_FRAME& stereo) {
	PostFromEngine([this, stereo]() {
		stereo_meter->AddPoints(stereo.points);
		if(stereo.valid)
			correlation_meter->SetCorrelation(stereo.correlation);
	});
}

void EDIScopeQt::EnsembleEDITiming(const EDI_TIMING& timing) {
	PostFromEngine([this, timing]() {OnEDITiming(timing);});
}


// --- FICDecoderObserver (player thread) -----------------------------------------------------------------
void EDIScopeQt::FICChangeEnsemble(const FIC_ENSEMBLE& new_ensemble) {
	PostFromEngine([this, new_ensemble]() {OnChangeEnsemble(new_ensemble);});

	// update LTO for date/time + show
	if(dt_lto != new_ensemble.lto) {
		dt_lto = new_ensemble.lto;
		ShowDateTime(false);
	}
}

void EDIScopeQt::FICChangeService(const LISTED_SERVICE& service) {
	PostFromEngine([this, service]() {OnChangeService(service);});
}

void EDIScopeQt::FICRemoveService(const fic_service_id_t& service_id) {
	PostFromEngine([this, service_id]() {OnRemoveService(service_id);});
}

void EDIScopeQt::FICChangeSubchannel(int subchid, const FIC_SUBCHANNEL& subchannel) {
	PostFromEngine([this, subchid, subchannel]() {
		known_subchannels[subchid] = subchannel;
		RebuildDataSubchannelRows();
		UpdateCUBar();
		UpdateAnnouncementSubchannels();
	});
}

void EDIScopeQt::FICRemoveSubchannel(int subchid) {
	PostFromEngine([this, subchid]() {
		known_subchannels.erase(subchid);
		RebuildDataSubchannelRows();
		UpdateCUBar();
		UpdateAnnouncementSubchannels();
	});
}

void EDIScopeQt::FICChangeDataServiceSID(int subchid, uint32_t sid) {
	PostFromEngine([this, subchid, sid]() {
		known_data_service_sids[subchid] = sid;
		RebuildDataSubchannelRows();
		UpdateCUBar();
	});
}

void EDIScopeQt::FICRemoveDataServiceSID(int subchid) {
	PostFromEngine([this, subchid]() {
		known_data_service_sids.erase(subchid);
		RebuildDataSubchannelRows();
		UpdateCUBar();
	});
}

void EDIScopeQt::FICChangeDataServiceUAType(uint32_t sid, int ua_type) {
	PostFromEngine([this, sid, ua_type]() {
		known_data_service_ua_types[sid] = ua_type;
		RebuildDataSubchannelRows();
		services_model->RefreshAll();
	});
}

void EDIScopeQt::FICChangeSPIService(int subchid, int address, bool fec) {
	PostFromEngine([this, subchid, address, fec]() {OnChangeSPIService(subchid, address, fec);});
}

void EDIScopeQt::FICRemoveSPIService(int subchid) {
	PostFromEngine([this, subchid]() {OnRemoveSPIService(subchid);});
}

void EDIScopeQt::FICChangeServiceLinking(int key, const FIC_SERVICE_LINKING_SET& set) {
	PostFromEngine([this, key, set]() {
		service_linking_sets[key] = set;
		if(links_window->isVisible()) {
			links_window->UpdateServiceLinking(key, set);
			// a linking set can name services not carried on this ensemble - listed as placeholders
			links_window->SetServices(BuildLinksServiceList());
		}
	});
}

void EDIScopeQt::FICChangeFrequencyInfo(int key, const FIC_FREQUENCY_INFO& info) {
	PostFromEngine([this, key, info]() {
		frequency_infos[key] = info;
		if(links_window->isVisible())
			links_window->UpdateFrequencyInfo(key, info);
	});
}

void EDIScopeQt::FICChangeOEService(int64_t key, const FIC_OE_SERVICE& service) {
	PostFromEngine([this, key, service]() {
		oe_services[key] = service;
		if(links_window->isVisible())
			links_window->UpdateOEService(key, service);
	});
}

void EDIScopeQt::FICChangeOEAnnouncementSupport(const FIC_OE_ANNOUNCEMENT_SUPPORT& support) {
	PostFromEngine([this, support]() {ann_window->UpdateOESupport(support);});
}

void EDIScopeQt::FICChangeOEAnnouncementSwitching(uint32_t key, const FIC_OE_ASW_CLUSTER& cluster) {
	PostFromEngine([this, key, cluster]() {
		oe_asw_clusters[key] = cluster;
		ann_window->UpdateOESwitching(key, cluster);
		UpdateAlarm();
	});
}

void EDIScopeQt::FICChangeUTCDateTime(const FIC_DAB_DT& utc_dt) {
	// (re)sync to DAB date/time + update/show/reschedule
	utc_dt_next = utc_dt;
	dt_update = std::chrono::steady_clock::now();

	if(dt_lto != FIC_ENSEMBLE::lto_none) {
		std::string dt_str = FICDecoder::ConvertDateTimeToString(utc_dt, dt_lto, true);
		PostFromEngine([this, dt_str]() {
			status_datetime->setToolTip("Local date/time (from the ensemble)\nLast synced: " + QString::fromStdString(dt_str));
		});
	}

	ShowDateTime(true);
}

void EDIScopeQt::FICDiscardedFIB() {
	discarded_fibs++;
}


// --- PADDecoderObserver (player thread) -----------------------------------------------------------------
void EDIScopeQt::PADChangeDynamicLabel(const DL_STATE& dl) {
	PostFromEngine([this, dl]() {OnChangeDynamicLabel(dl);});
}

void EDIScopeQt::PADChangeSlide(const MOT_FILE& slide) {
	PostFromEngine([this, slide]() {slideshow->UpdateSlide(slide);});
}

void EDIScopeQt::PADFileProgress(const double fraction) {
	PostFromEngine([this, fraction]() {slideshow->UpdateFileProgress(fraction);});
}

void EDIScopeQt::PADLengthError(size_t /*announced_xpad_len*/, size_t /*xpad_len*/) {
	fprintf(stderr, "\x1B[31m" "[X-PAD len]" "\x1B[0m" " ");
}


// --- MOTDirectoryDecoderObserver (player thread) -----------------------------------------------------------------
void EDIScopeQt::SPIObjectCompleted(const MOT_DIR_OBJECT& info, const std::vector<uint8_t>& data) {
	// ETSI TS 102 371 table 11: ContentSubType 0 = Service Information, 1 = Programme Information,
	// 2 = Group Information
	switch(info.content_sub_type) {
	case 0: {
		SPI_SERVICE_INFORMATION si;
		if(SPIDecoder::DecodeServiceInformation(data, si))
			PostFromEngine([this, si]() {
				spi_info = si;
				services_model->RefreshAll();	// refreshes the S column's checkmarks
				if(spi_window->isVisible())
					spi_window->Update(spi_info, spi_logo_data);
			});
		break;
	}
	case 1: {
		SPI_SCHEDULE schedule;
		if(SPIDecoder::DecodeSchedule(data, schedule)) {
			// some encoders don't embed a <scope><serviceScope> in the schedule itself - fall
			// back to the MOT ScopeID parameter to still know which service this is for
			if(!schedule.has_scope_service)
				schedule.has_scope_service = SPIDecoder::DecodeScopeId(info.scope_id, schedule.ecc, schedule.eid, schedule.sid, schedule.sid_32bit, schedule.scids);
			// last resort: the ContentName convention
			if(!schedule.has_scope_service)
				schedule.has_scope_service = TryExtractSidFromLegacyContentName(info.content_name, schedule.sid, schedule.scids);

			SPIDecoder::DecodeScopeTime(info.scope_start, schedule.scope_start);
			SPIDecoder::DecodeScopeTime(info.scope_end, schedule.scope_end);
			// fall back to the first programme with a time, if the MOT ScopeStart parameter is missing
			if(!schedule.scope_start.valid)
				for(const SPI_PROGRAMME& p : schedule.programmes)
					if(p.time.valid) {
						schedule.scope_start = p.time;
						break;
					}

			PostFromEngine([this, schedule]() {
				if(schedule.has_scope_service) {
					spi_schedules[std::make_pair(schedule.sid, SPIDecoder::FormatDate(schedule.scope_start))] = schedule;
					services_model->RefreshAll();	// refreshes the P column's checkmarks
				}
				if(spi_window->isVisible())
					spi_window->UpdateSchedule(schedule);
			});
		}
		break;
	}
	case 2: {
		SPI_GROUP_INFORMATION gi;
		if(SPIDecoder::DecodeGroupInformation(data, gi))
			PostFromEngine([this, gi]() {
				spi_group_info = gi;
				if(spi_window->isVisible())
					spi_window->UpdateGroupInformation(spi_group_info, spi_logo_data);
			});
		break;
	}
	}
}

void EDIScopeQt::SPIOtherObjectCompleted(const MOT_DIR_OBJECT& info, const std::vector<uint8_t>& data) {
	// expected: a service/ensemble logo (ContentType=image, JFIF or PNG)
	if(info.content_type != MOT_FILE::CONTENT_TYPE_IMAGE)
		return;

	std::string content_name = info.content_name;
	PostFromEngine([this, content_name, data]() {
		spi_logo_data[content_name] = data;
		if(spi_window->isVisible()) {
			spi_window->Update(spi_info, spi_logo_data);
			spi_window->UpdateGroupInformation(spi_group_info, spi_logo_data);	// groups can have logos too
		}
	});
}
