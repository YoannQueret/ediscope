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

#ifndef EDISCOPE_QT_H_
#define EDISCOPE_QT_H_

// eti_player.h, that indirectly includes SDL.h, must be included before <string> or it won't compile on OS X
#include "eti_player.h"

#include <atomic>
#include <chrono>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTableView>
#include <QTimer>
#include <QToolButton>

#include "ediscope_qt_theme.h"
#include "ediscope_qt_services_model.h"
#include "ediscope_qt_cu_bar.h"
#include "ediscope_qt_dl_plus.h"
#include "ediscope_qt_sls.h"
#include "ediscope_qt_meters.h"
#include "ediscope_qt_links.h"
#include "ediscope_qt_rdns.h"
#include "ediscope_qt_spi.h"
#include "ediscope_qt_fig.h"
#include "ediscope_qt_announcements.h"
#include "ediscope_qt_mnsc.h"
#include "ediscope_qt_image_archive.h"
#include "ediscope_qt_margin.h"
#include "mnsc_decoder.h"
#include "fig_analyzer.h"
#include "eti_source.h"
#include "edi_source.h"
#include "edi_player.h"
#include "fic_decoder.h"
#include "pad_decoder.h"
#include "msc_packet_demux.h"
#include "mot_directory_decoder.h"
#include "spi_decoder.h"
#include "radiodns.h"
#include "tools.h"
#include "version.h"


// --- EDIScopeQtOptions -----------------------------------------------------------------
struct EDIScopeQtOptions {
	std::string filename;
	std::string source_format;
	bool initial_first_found_service;
	std::string initial_label;
	int initial_sid;
	int initial_scids;
	std::string recordings_path;
	size_t rec_prebuffer_size_s;
	bool pcm_output;
	bool wav_output;
	bool untouched_output;
	bool disable_int_catch_up;
	bool loose;
	bool disable_dyn_fic_msgs;
	bool recordings_path_given;	// -r on the command line: overrides the saved setting for this run

	EDIScopeQtOptions() :
		source_format(EnsembleSource::FORMAT_ETI),
		initial_first_found_service(false),
		initial_sid(LISTED_SERVICE::sid_none),
		initial_scids(LISTED_SERVICE::scids_none),
		recordings_path("/tmp"),
		rec_prebuffer_size_s(0),
		pcm_output(false),
		wav_output(false),
		untouched_output(false),
		disable_int_catch_up(false),
		loose(false),
		disable_dyn_fic_msgs(false),
		recordings_path_given(false)
		{}
};


// --- EDIScopeQtRecSample -----------------------------------------------------------------
struct EDIScopeQtRecSample {
	std::vector<uint8_t> data;
	time_t ts;
	size_t duration_ms;

	EDIScopeQtRecSample(const uint8_t* data, size_t len, size_t duration_ms) : data(data, data + len), duration_ms(duration_ms) {
		ts = time(nullptr);
		if(ts == (time_t) -1)
			perror("EDIScopeQt: error while getting time for sample");
	}
};


// --- EDIScopeQt -----------------------------------------------------------------
// the main window. The engine (source, player, decoders) reports through the observer interfaces
// below from its own threads; every report is handed over to the UI thread through Qt's event
// queue (PostToUI) before touching any widget or UI-side state
class EDIScopeQt : public QMainWindow, EnsembleSourceObserver, EnsemblePlayerObserver, FICDecoderObserver, PADDecoderObserver, UntouchedStreamConsumer, MOTDirectoryDecoderObserver, MNSCDecoderObserver {
	Q_OBJECT
private:
	EDIScopeQtOptions options;

	std::string switch_service_label;
	int switch_service_sid;
	int switch_service_scids;

	EnsembleSource *ensemble_source;
	std::thread ensemble_source_thread;

	EnsemblePlayer *ensemble_player;
	AudioOutputType audio_output_type;
	void EnsureEnsemblePlayer(const std::string& format);

	FICDecoder *fic_decoder;
	PADDecoder *pad_decoder;

	// hands an engine report over to the UI thread. Reports still on their way when the source is
	// stopped (StopEnsembleSource bumps engine_generation) are dropped there, so nothing from a
	// previous connection reappears after the reset - e.g. a service of the previous ensemble
	std::atomic<unsigned int> engine_generation;
	template<typename F> void PostFromEngine(F&& f) {
		unsigned int generation = engine_generation;
		QMetaObject::invokeMethod(this, [this, generation, f = std::forward<F>(f)]() mutable {
			if(generation == engine_generation)
				f();
		}, Qt::QueuedConnection);
	}

	// connection state, shown in the status bar
	std::atomic<int64_t> last_frame_ms;	// steady clock, 0 = no frame received yet
	std::atomic<unsigned int> discarded_fibs;
	// a network URI only joins the history once its first frame has actually been received, so a
	// mistyped or unreachable one never ends up in there
	std::string history_pending_uri;
	std::atomic<bool> first_frame_received;
	QString source_uri;

	// recording
	std::mutex rec_mutex;
	FILE* rec_file;
	std::string rec_filename;
	long int rec_duration_ms;
	std::list<EDIScopeQtRecSample> rec_prebuffer;
	long int rec_prebuffer_filled_ms;
	void UpdateRecStatus(bool decoding);	// rec_mutex must already be locked

	// full ensemble recording: the frames as received (EDI AF packets or ETI-NI frames), written
	// on the source thread
	std::mutex mux_rec_mutex;
	FILE* mux_rec_file;
	std::string mux_rec_filename;
	std::atomic<unsigned long> mux_rec_frames;
	bool mux_rec_convert_to_eti;		// EDI source recorded as ETI-NI
	unsigned long mux_rec_convert_errors;
	std::vector<uint8_t> mux_rec_eti_frame;

	// at most one recording at a time; started from the Record menu, stopped from there or the toolbar
	enum Recording {REC_NONE, REC_SERVICE, REC_ENSEMBLE} recording;
	time_t recording_start;
	QString recording_type;
	static QString RecordingStartString(time_t start);
	static std::string CleanFileName(std::string name);
	void StartServiceRecording();
	void StartEnsembleRecording(bool as_eti);
	void StopRecording();
	void ShowRecordingSummary(const QString& filename, long int duration_ms);
	void UpdateRecordingUI();
	void UpdateEnsembleRecordingStatus();
	void ShowSettings();
	void ApplyTheme(EDIScopeQtTheme::Theme theme);
	static QStringList ToolbarShortcuts();	// "links", "rdns", "spi", "ann", "mnsc", "fig" - 3 at most
	void ApplyToolbarShortcuts();
	std::map<QString, QAction*> shortcut_actions;	// the toolbar's widget action of each shortcut button

	// FIG repetition analysis - started the first time its window is opened for the current source
	// (or right away, if the window is open when a source starts), then runs until the source
	// changes, whether the window is open or not
	FIGAnalyzer fig_analyzer;
	std::atomic<bool> fig_analysis_running{false};
	void StartFIGAnalysis();
	QPushButton *btn_fig;
	QAction *action_fig;
	EDIScopeQtFIGWindow *fig_window;
	void OnFIGToggled(bool checked);

	// announcements (FIG 0/18, 0/19, 0/25, 0/26) - the window is fed all the time, so its history
	// and checks cover the whole connection
	QPushButton *btn_ann;
	QAction *action_ann;
	EDIScopeQtAnnouncementsWindow *ann_window;
	fic_oe_asw_clusters_t oe_asw_clusters;	// for the alarm badge (alarm of another ensemble)
	void OnAnnouncementsToggled(bool checked);
	void UpdateAnnouncementServices();
	void UpdateAnnouncementSubchannels();
	void UpdateAlarm();

	// MNSC (ETS 300 799): transmitter set-up and control information - the window is fed all the
	// time, like the announcements one
	MNSCDecoder *mnsc_decoder;
	QPushButton *btn_mnsc;
	QAction *action_mnsc;
	EDIScopeQtMNSCWindow *mnsc_window;
	void OnMNSCToggled(bool checked);

	// saving of every image received (slides, SPI and RadioDNS logos), as set in the settings
	EDIScopeQtImageArchive image_archive;
	std::atomic<int> slide_service_sid{LISTED_SERVICE::sid_none};	// SId of the service the slides come from
	void ArchiveSPILogos();
	void ArchiveRDNSLogos(int eid, int sid, const RDNS_RESULT& result);

	// new version check (GitHub's latest release), at startup only - shown in a banner above the
	// services, until closed; a version closed is not shown again
	QFrame *update_banner;
	QLabel *update_label;
	QString update_version;
	void CheckForNewVersion();
	void ShowNewVersion(const QString& version);

	// everything known about the ensemble/multiplex, as selectable text - opened by a click on the
	// services title
	EDI_TIMING last_edi_timing;

	// the EDI margin over the last 3 hours, recorded all the time - shown as a graph by a click on it
	EDIScopeQtMarginGraph::history_t margin_history;
	// the first values after the margin becomes available (new source, reconnection) aren't settled
	// yet: recorded only from MARGIN_SETTLE_MS after that on
	static const int64_t MARGIN_SETTLE_MS = 3000;
	int64_t margin_valid_since_ms = 0;	// 0: no valid margin (yet/anymore)
	int64_t margin_last_ms = 0;			// the last valid margin received - none for 2 s: it settles again
	void ShowMarginGraph();
	void ShowEnsembleInfo();
	QString EnsembleInfoText(QString *label = nullptr);

	// date/time
	FIC_DAB_DT utc_dt_curr;
	int dt_lto;
	std::string dt_str_prev;
	FIC_DAB_DT utc_dt_next;
	std::chrono::steady_clock::time_point dt_update;
	void ShowDateTime(bool scheduled);

	// ensemble/services state
	FIC_ENSEMBLE ensemble;
	AUDIO_SERVICE_FORMAT current_audio_format;
	int current_audio_format_subchid;
	std::map<int, FIC_SUBCHANNEL> known_subchannels;	// every sub-channel in the ensemble, audio or data - for accurate CU bar occupancy
	std::map<int, uint32_t> known_data_service_sids;	// SubChId -> SId, for data sub-channels whose service identity is known
	std::map<uint32_t, int> known_data_service_ua_types;	// SId -> UAtype (FIG 0/13, P/D=1), for data services whose carried application is known
	bool edi_timing_valid;

	// SPI - decoded independently of its window being open (the services table's S/P columns use it too)
	int spi_subchid;
	MSCPacketDemux *spi_demux;
	MOTDirectoryDecoder *spi_mot_decoder;
	SPI_SERVICE_INFORMATION spi_info;
	std::map<std::string, std::vector<uint8_t>> spi_logo_data;
	std::map<std::pair<uint32_t, std::string>, SPI_SCHEDULE> spi_schedules;	// keyed by (service SId, "YYYY-MM-DD")
	SPI_GROUP_INFORMATION spi_group_info;
	std::map<uint32_t, std::string> known_service_names;
	void StopSPIService();

	// RadioDNS (ETSI TS 103 270): every audio service is tested automatically as soon as its
	// broadcast parameters are known - the services table's R column shows the outcome
	std::map<std::pair<int,int>, RDNS_RESULT> rdns_results;	// (SId, SCIdS) -> last known result
	std::map<std::pair<int,int>, std::pair<int,int>> rdns_tested_params;	// (SId, SCIdS) -> (ECC, EId) it was last (re)tested with
	std::set<std::pair<int,int>> rdns_pending;	// (SId, SCIdS) currently being tested
	std::vector<EDIScopeQtRDNSWindow::SERVICE_ENTRY> BuildRDNSServiceList() const;
	void StartRDNSTest(int sid, int scids, int ecc, int eid);
	void MaybeStartRDNSTest(int sid, int scids, int ecc, int eid);
	void RescanRDNSServices();

	// Service linking (FIG 0/6), Frequency Information (FIG 0/21) and OE Services (FIG 0/24) - kept
	// here (not just in the Links window) so they survive the window being hidden/shown again
	fic_service_linking_sets_t service_linking_sets;
	fic_frequency_infos_t frequency_infos;
	fic_oe_services_t oe_services;

	// widgets
	QComboBox *combo_uri;
	QPushButton *btn_connect;
	QPushButton *btn_record;
	QLabel *label_record;
	QAction *action_record_service;
	QAction *action_record_edi;
	QAction *action_record_eti;
	QAction *action_stop_recording;
	QPushButton *btn_links;
	QAction *action_links;
	EDIScopeQtLinksWindow *links_window;
	void OnLinksToggled(bool checked);
	std::vector<EDIScopeQtLinksWindow::SERVICE_ENTRY> BuildLinksServiceList() const;
	QPushButton *btn_rdns;
	QAction *action_rdns;
	EDIScopeQtRDNSWindow *rdns_window;
	void OnRDNSToggled(bool checked);
	QPushButton *btn_spi;
	QAction *action_spi;
	EDIScopeQtSPIWindow *spi_window;
	void OnSPIToggled(bool checked);
	QToolButton *btn_stop;	// stops playing the selected service (deselects it)
	void UpdateStopButton();
	QToolButton *btn_mute;
	QSlider *slider_volume;

	EDIScopeQtPanel *panel_services;
	EDIScopeQtServicesModel *services_model;
	EDIScopeQtServicesSortProxy *services_proxy;
	QTableView *services_view;
	EDIScopeQtCUBar *cu_bar;
	QLabel *label_cu_usage;

	EDIScopeQtPanel *panel_dl;
	QLabel *label_dl;
	EDIScopeQtPanel *panel_dl_plus;
	EDIScopeQtDLPlusTable *dl_plus;
	EDIScopeQtSlideshow *slideshow;
	EDIScopeQtStereoMeter *stereo_meter;
	EDIScopeQtCorrelationMeter *correlation_meter;
	EDIScopeQtSpectrum *spectrum;
	EDIScopeQtLevelMeter *level_meter;

	QLabel *status_connection;
	QLabel *status_ensemble;
	QLabel *status_datetime;
	QLabel *status_edi_delay;
	EDIScopeQtIconButton *btn_margin_graph;		// next to the margin: its graph
	EDIScopeQtIconButton *btn_ensemble_info;	// next to the services title: the ensemble's details
	QLabel *status_asu;
	QLabel *status_fib;
	QProgressBar *progress_position;
	QTimer status_timer;

	void InitWidgets();
	void InitMenus();
	QWidget* InitToolBar();

	// services table
	std::string ServiceDetailCellText(const LISTED_SERVICE& service, int column) const;
	std::string ServiceToolTip(const LISTED_SERVICE& service) const;
	int ServicesTableColumnCompare(const LISTED_SERVICE& a, const LISTED_SERVICE& b, int column) const;
	void OnServicesHeaderClicked(int column);
	bool GetSelectedService(LISTED_SERVICE& service) const;
	int GetSelectedServiceIndex() const;
	void SelectSourceRow(int row);
	void TrySwitchService(int index);
	void RebuildDataSubchannelRows();
	void UpdateCUBar();

	void SetService(const LISTED_SERVICE& service);
	void UpdateAnnouncementSupport(const LISTED_SERVICE& service);
	void UpdateEnsembleDisplay();
	void UpdateConnectionStatus();

	// source handling
	void StartSource(EnsembleSource *source, const QString& uri);
	void StopEnsembleSource();
	void ShowEnsembleWaitingMessage(bool file);	// file: no timestamps to wait for
	void OnConnectToggled(bool checked);
	void OnUriActivated(int index);
	void OpenFile();
	void AddToNetworkHistory(const std::string& uri);

	void OnMuteToggled(bool checked);
	void OnVolumeChanged(int value);
	void ShowAbout();

	bool HandleKey(QKeyEvent *key_event);

	// engine reports, on the UI thread
	void OnChangeFormat(const AUDIO_SERVICE_FORMAT& format);
	void OnEDITiming(const EDI_TIMING& timing);
	void OnChangeEnsemble(const FIC_ENSEMBLE& new_ensemble);
	void OnChangeService(const LISTED_SERVICE& service);
	void OnRemoveService(const fic_service_id_t& id);
	void OnChangeSPIService(int subchid, int address, bool fec);
	void OnRemoveSPIService(int subchid);
	void OnChangeDynamicLabel(const DL_STATE& dl);

	// EnsembleSourceObserver (source thread)
	void EnsembleProcessFrame(const uint8_t *data) override;
	void EnsembleUpdateProgress(const ENSEMBLE_PROGRESS& progress) override;
	void EnsembleDoRegularWork() override;

	// EnsemblePlayerObserver (player threads)
	void EnsembleChangeFormat(const AUDIO_SERVICE_FORMAT& format) override;
	void EnsembleAudioLevel(const std::vector<double>& channel_dbfs) override;
	void EnsembleAudioSpectrum(const AUDIO_SPECTRUM& audio_spectrum) override;
	void EnsembleAudioStereo(const AUDIO_STEREO_FRAME& stereo) override;
	void EnsembleEDITiming(const EDI_TIMING& timing) override;
	void EnsembleProcessFIC(const uint8_t *data, size_t len) override {
		fic_decoder->Process(data, len);
		if(fig_analysis_running)
			fig_analyzer.ProcessFIC(data, len);
	}
	void EnsembleProcessMNSC(int fp, const uint8_t *mnsc) override {mnsc_decoder->Process(fp, mnsc);}
	void EnsembleProcessPAD(const uint8_t *xpad_data, size_t xpad_len, bool exact_xpad_len, const uint8_t* fpad_data) override {pad_decoder->Process(xpad_data, xpad_len, exact_xpad_len, fpad_data);}

	// UntouchedStreamConsumer (player thread)
	void ProcessUntouchedStream(const uint8_t* data, size_t len, size_t duration_ms) override;

	// FICDecoderObserver (player thread)
	void FICChangeEnsemble(const FIC_ENSEMBLE& ensemble) override;
	void FICChangeService(const LISTED_SERVICE& service) override;
	void FICRemoveService(const fic_service_id_t& service_id) override;
	void FICChangeSubchannel(int subchid, const FIC_SUBCHANNEL& subchannel) override;
	void FICRemoveSubchannel(int subchid) override;
	void FICChangeDataServiceSID(int subchid, uint32_t sid) override;
	void FICRemoveDataServiceSID(int subchid) override;
	void FICChangeDataServiceUAType(uint32_t sid, int ua_type) override;
	void FICChangeSPIService(int subchid, int address, bool fec) override;
	void FICRemoveSPIService(int subchid) override;
	void FICChangeServiceLinking(int key, const FIC_SERVICE_LINKING_SET& set) override;
	void FICChangeFrequencyInfo(int key, const FIC_FREQUENCY_INFO& info) override;
	void FICChangeOEService(int64_t key, const FIC_OE_SERVICE& service) override;
	void FICChangeOEAnnouncementSupport(const FIC_OE_ANNOUNCEMENT_SUPPORT& support) override;
	void FICChangeOEAnnouncementSwitching(uint32_t key, const FIC_OE_ASW_CLUSTER& cluster) override;
	void FICChangeUTCDateTime(const FIC_DAB_DT& utc_dt) override;
	void FICDiscardedFIB() override;

	// PADDecoderObserver (player thread)
	void PADChangeDynamicLabel(const DL_STATE& dl) override;
	void PADChangeSlide(const MOT_FILE& slide) override;
	void PADFileProgress(const double fraction) override;
	void PADLengthError(size_t announced_xpad_len, size_t xpad_len) override;

	// MNSCDecoderObserver (player thread)
	void MNSCTSCMessage(const MNSC_TSC& tsc) override;
	void MNSCCRCError() override;

	// MOTDirectoryDecoderObserver (player thread)
	void SPIObjectCompleted(const MOT_DIR_OBJECT& info, const std::vector<uint8_t>& data) override;
	void SPIOtherObjectCompleted(const MOT_DIR_OBJECT& info, const std::vector<uint8_t>& data) override;
protected:
	void closeEvent(QCloseEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;
public:
	EDIScopeQt(EDIScopeQtOptions options);
	~EDIScopeQt();
};

#endif /* EDISCOPE_QT_H_ */
