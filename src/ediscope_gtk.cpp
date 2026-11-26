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

#include "ediscope_gtk.h"

#include "ecc_countries.h"
#include "ediscope_icon.h"
#include "edi_network_source.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <fstream>
#include <sys/stat.h>

static EDIScopeGTK *ediscope = nullptr;

// --- network connect history (persisted to $XDG_CONFIG_HOME/ediscope/network_history, or
// ~/.config/ediscope/network_history) -----------------------------------------------------------
static const size_t NETWORK_HISTORY_MAX_ENTRIES = 10;

// a sentinel entry at the top of the network/file URI combo's dropdown - picking it opens a
// native file chooser instead of being treated as a literal URI (see on_combo_network_uri_changed())
static const std::string NETWORK_URI_BROWSE_ITEM = "Open EDI/ETI File...";

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
		// file:// sources aren't kept in history (see on_tglbtn_network_connect()) - also
		// weeded out here in case an older version of this file still has some
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

static void break_handler(int) {
	fprintf(stderr, "...EDIScope exits...\n");
	if(ediscope)
		ediscope->hide();
}


static void usage(const char* exe) {
	EDIScopeGTKOptions options_default;

	fprint_ediscope_banner(stderr);
	fprintf(stderr, "Usage: %s [OPTIONS] [file]\n", exe);
	fprintf(stderr, "  -h           Show this help\n"
					"  -f <format>  Source format: \"%s\" (default), \"%s\"\n"
					"  -l <label>   Label of the service to be played\n"
					"  -1           Play the first service found\n"
					"  -s <sid>     ID of the service to be played\n"
					"  -x <scids>   ID of the service component to be played (requires service ID)\n"
					"  -r <path>    Path for recordings (default: %s)\n"
					"  -P <size>    Recording prebuffer size in seconds (default: %zu)\n"
					"  -p           Output raw PCM to stdout instead of using SDL\n"
					"  -w           Output RIFF WAVE with PCM to stdout instead of using SDL (useful only on Little Endian)\n"
					"  -u           Output untouched audio stream to stdout instead of using SDL\n"
					"  -I           Don't catch up on stream after interruption\n"
					"  -L           Enable loose behaviour (e.g. PAD conformance)\n"
					"  -F           Disable dynamic FIC messages (dynamic PTY, announcements)\n"
					"  file         Input file to be played (stdin, if not specified) - a file:// URI works too\n",
					EnsembleSource::FORMAT_ETI.c_str(),
					EnsembleSource::FORMAT_EDI.c_str(),
					options_default.recordings_path.c_str(),
					options_default.rec_prebuffer_size_s
			);
	exit(1);
}


int main(int argc, char **argv) {
	// handle signals
	if(signal(SIGINT, break_handler) == SIG_ERR) {
		perror("EDIScope: error while setting SIGINT handler");
		return 1;
	}
	if(signal(SIGTERM, break_handler) == SIG_ERR) {
		perror("EDIScope: error while setting SIGTERM handler");
		return 1;
	}

	EDIScopeGTKOptions options;
	int initial_param_count = 0;

	// option args
	int c;
	while((c = getopt(argc, argv, "hf:l:r:P:s:x:1pwuILF")) != -1) {
		switch(c) {
		case 'h':
			usage(argv[0]);
			break;
		case 'f':
			options.source_format = optarg;
			break;
		case 'l':
			options.initial_label = optarg;
			initial_param_count++;
			break;
		case '1':
			options.initial_first_found_service = true;
			initial_param_count++;
			break;
		case 's':
			options.initial_sid = strtol(optarg, nullptr, 0);
			initial_param_count++;
			break;
		case 'x':
			options.initial_scids = strtol(optarg, nullptr, 0);
			break;
		case 'r':
			options.recordings_path = optarg;
			break;
		case 'P':
			options.rec_prebuffer_size_s = strtol(optarg, nullptr, 0);
			break;
		case 'p':
			options.pcm_output = true;
			break;
		case 'w':
			options.wav_output = true;
			break;
		case 'u':
			options.untouched_output = true;
			break;
		case 'I':
			options.disable_int_catch_up = true;
			break;
		case 'L':
			options.loose = true;
			break;
		case 'F':
			options.disable_dyn_fic_msgs = true;
			break;
		case '?':
		default:
			usage(argv[0]);
		}
	}

	// non-option args
	switch(argc - optind) {
	case 0:
		break;
	case 1:
		options.filename = StringTools::FilePathFromArg(argv[optind]);
		break;
	default:
		usage(argv[0]);
	}

	// ensure valid options
	if(options.initial_scids != LISTED_SERVICE::scids_none && options.initial_sid == LISTED_SERVICE::sid_none) {
		fprintf(stderr, "The service component ID requires the service ID to be specified!\n");
		usage(argv[0]);
	}
#ifdef EDISCOPE_DISABLE_SDL
	if(!options.pcm_output) {
		fprintf(stderr, "SDL output was disabled, so PCM output must be selected!\n");
		usage(argv[0]);
	}
#endif

	if(options.pcm_output + options.wav_output + options.untouched_output > 1) {
		fprintf(stderr, "No more than one output option can be specified!\n");
		usage(argv[0]);
	}


	// at most one param needed!
	if(initial_param_count > 1) {
		fprintf(stderr, "At most one initial parameter shall be specified!\n");
		usage(argv[0]);
	}


	fprint_ediscope_banner(stderr);

	int myargc = 1;
	Glib::RefPtr<Gtk::Application> app = Gtk::Application::create(myargc, argv, "");

	// fixed app icon (taskbar/window list) for every window this app creates - the main window,
	// and any dialog that doesn't set its own icon (error dialogs, the logs window)
	Glib::RefPtr<Gdk::PixbufLoader> icon_loader = Gdk::PixbufLoader::create();
	icon_loader->write(ediscope_icon_png, ediscope_icon_png_len);
	icon_loader->close();
	Glib::RefPtr<Gdk::Pixbuf> icon_pixbuf = icon_loader->get_pixbuf();
	Gtk::Window::set_default_icon(icon_pixbuf);

	ediscope = new EDIScopeGTK(options);
	ediscope->set_icon(icon_pixbuf);
	int result = app->run(*ediscope);
	delete ediscope;

	return result;
}



// --- EDIScopeGTK -----------------------------------------------------------------
EDIScopeGTK::EDIScopeGTK(EDIScopeGTKOptions options) : combo_network_uri(true) {
	this->options = options;

	switch_service_label = options.initial_label;
	switch_service_sid = options.initial_sid;
	switch_service_scids = options.initial_scids;
	switch_service_applied = false;

	rec_file = nullptr;
	rec_duration_ms = 0;
	rec_prebuffer_filled_ms = 0;

	dt_lto = FIC_ENSEMBLE::lto_none;

	current_audio_format_subchid = AUDIO_SERVICE::subchid_none;

	logs_window.set_transient_for(*this);
	logs_window.signal_delete_event().connect([this](GdkEventAny*) -> bool {
		tglbtn_logs.set_active(false);
		return true;
	});
	spi_window.set_transient_for(*this);
	// closing it via the window manager (its own close button) just hides it and un-toggles the
	// "SPI" button, instead of destroying the window - same as toggling that button off
	spi_window.signal_delete_event().connect([this](GdkEventAny*) -> bool {
		tglbtn_spi.set_active(false);
		return true;
	});
	rdns_window.set_transient_for(*this);
	rdns_window.signal_delete_event().connect([this](GdkEventAny*) -> bool {
		tglbtn_rdns.set_active(false);
		return true;
	});
	rdns_window.SetTestRequestedSlot([this](int sid, int scids, int ecc, int eid) {StartRDNSTest(sid, scids, ecc, eid);});
	links_window.set_transient_for(*this);
	links_window.signal_delete_event().connect([this](GdkEventAny*) -> bool {
		tglbtn_links.set_active(false);
		return true;
	});

	ensemble_update_progress.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::EnsembleUpdateProgressEmitted));
	ensemble_change_format.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::EnsembleChangeFormatEmitted));
	ensemble_audio_level.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::EnsembleAudioLevelEmitted));
	ensemble_audio_spectrum.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::EnsembleAudioSpectrumEmitted));
	ensemble_audio_stereo.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::EnsembleAudioStereoEmitted));
	ensemble_edi_timing.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::EnsembleEDITimingEmitted));
	fic_change_ensemble.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICChangeEnsembleEmitted));
	fic_change_service.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICChangeServiceEmitted));
	fic_remove_service.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICRemoveServiceEmitted));
	fic_change_subchannel.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICChangeSubchannelEmitted));
	fic_remove_subchannel.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICRemoveSubchannelEmitted));
	fic_change_data_service_sid.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICChangeDataServiceSIDEmitted));
	fic_remove_data_service_sid.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICRemoveDataServiceSIDEmitted));
	fic_change_data_service_ua_type.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICChangeDataServiceUATypeEmitted));
	fic_change_spi_service.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICChangeSPIServiceEmitted));
	fic_remove_spi_service.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICRemoveSPIServiceEmitted));
	spi_info_updated.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::SPIInfoUpdatedEmitted));
	spi_logo_updated.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::SPILogoUpdatedEmitted));
	spi_schedule_updated.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::SPIScheduleUpdatedEmitted));
	spi_group_info_updated.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::SPIGroupInfoUpdatedEmitted));
	rdns_result_ready.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::RDNSResultReadyEmitted));
	fic_change_service_linking.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICChangeServiceLinkingEmitted));
	fic_change_frequency_info.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICChangeFrequencyInfoEmitted));
	fic_change_oe_service.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICChangeOEServiceEmitted));
	fic_log_message.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::FICLogMessageEmitted));
	pad_change_dynamic_label.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::PADChangeDynamicLabelEmitted));
	pad_change_slide.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::PADChangeSlideEmitted));
	pad_file_progress.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::PADFileProgressEmitted));
	do_rec_status_update.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::DoRecStatusUpdateEmitted));
	do_datetime_sync.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::DoDateTimeSyncEmitted));
	do_datetime_update.GetDispatcher().connect(sigc::mem_fun(*this, &EDIScopeGTK::DoDateTimeUpdateEmitted));

	audio_output_type = AudioOutputType::SDL;
#ifndef EDISCOPE_DISABLE_SDL
	if(options.pcm_output)
#endif
		audio_output_type = AudioOutputType::PCM;
	if(options.wav_output)
		audio_output_type = AudioOutputType::WAV;
	if(options.untouched_output)
		audio_output_type = AudioOutputType::Untouched;

	if(options.source_format == EnsembleSource::FORMAT_ETI)
		ensemble_player = new ETIPlayer(audio_output_type, options.disable_int_catch_up, this);
	else
		ensemble_player = new EDIPlayer(audio_output_type, options.disable_int_catch_up, this);

	if(options.source_format == EnsembleSource::FORMAT_ETI)
		ensemble_source = new ETISource(options.filename, this);
	else {
		ensemble_source = new EDISource(options.filename, this);
		ShowEnsembleWaitingMessage();
	}
	ensemble_source_thread = std::thread(&EnsembleSource::Main, ensemble_source);

	fic_decoder = new FICDecoder(this, options.disable_dyn_fic_msgs);
	pad_decoder = new PADDecoder(this, options.loose);

	set_title("EDIScope v" + std::string(EDISCOPE_VERSION));

	InitWidgets();

	set_border_width(2 * WIDGET_SPACE);

	// add window key press event handler
	signal_key_press_event().connect(sigc::mem_fun(*this, &EDIScopeGTK::HandleKeyPressEvent));
	add_events(Gdk::KEY_PRESS_MASK);

	// add window config event handler (before default handler)
	add_events(Gdk::STRUCTURE_MASK);
}

EDIScopeGTK::~EDIScopeGTK() {
	// signal every worker thread to stop up front, before blocking on any single one of them: by
	// this point (app->run() has returned) the GTK main loop is gone for good, so if one thread's
	// join below takes a while, any OTHER thread left running unsignaled in the meantime could
	// still try to report through the observer (audio levels, EDI timing, FIC changes) and block
	// forever on a notification queue nothing will ever drain again
	if(ensemble_source)
		ensemble_source->DoExit();
	if(ensemble_player)
		ensemble_player->PrepareStop();

	if(ensemble_source) {
		ensemble_source_thread.join();
		delete ensemble_source;
	}

	delete ensemble_player;

	delete pad_decoder;
	delete fic_decoder;
}

int EDIScopeGTK::ServicesTableSlotCompare(const Gtk::TreeModel::iterator& a, const Gtk::TreeModel::iterator& b) {
	const LISTED_SERVICE& service_a = (LISTED_SERVICE) (*a)[services_table_cols.col_service];
	const LISTED_SERVICE& service_b = (LISTED_SERVICE) (*b)[services_table_cols.col_service];

	if(service_a < service_b)
		return -1;
	if(service_b < service_a)
		return 1;
	return 0;
}

int EDIScopeGTK::ServicesTableColumnCompare(int column_id, const Gtk::TreeModel::iterator& a, const Gtk::TreeModel::iterator& b) {
	const LISTED_SERVICE& sa = (LISTED_SERVICE) (*a)[services_table_cols.col_service];
	const LISTED_SERVICE& sb = (LISTED_SERVICE) (*b)[services_table_cols.col_service];

	switch(column_id) {
	case 0:	// SubChId
		return sa.subchid - sb.subchid;
	case 1:	// SID - sid_none (-1) sorts before every real SId
		return sa.sid - sb.sid;
	case 2:	// ECC - ecc_none (-1) sorts before every real ECC
		return sa.ecc - sb.ecc;
	case 5: {	// CUs - by start position; unknown (no sub-channel/protection decoded yet) sorts last
		int va = (!sa.subchannel.IsNone() && !sa.subchannel.pl.empty()) ? sa.subchannel.start : INT_MAX;
		int vb = (!sb.subchannel.IsNone() && !sb.subchannel.pl.empty()) ? sb.subchannel.start : INT_MAX;
		return va - vb; }
	case 7: {	// Bit rate - unknown sorts last
		int va = (!sa.subchannel.IsNone() && !sa.subchannel.pl.empty()) ? sa.subchannel.bitrate : INT_MAX;
		int vb = (!sb.subchannel.IsNone() && !sb.subchannel.pl.empty()) ? sb.subchannel.bitrate : INT_MAX;
		return va - vb; }
	case 9: {	// Samplerate (only known for the currently playing sub-channel) - unknown sorts last
		int va = (sa.is_audio && sa.audio_service.subchid == current_audio_format_subchid && current_audio_format.samplerate_khz) ? current_audio_format.samplerate_khz : INT_MAX;
		int vb = (sb.is_audio && sb.audio_service.subchid == current_audio_format_subchid && current_audio_format.samplerate_khz) ? current_audio_format.samplerate_khz : INT_MAX;
		return va - vb; }
	default:	// text-based columns (Label, Short label, Protection, Format, Audio mode, Language, PTY) - sort by the same text shown in the cell
		return ServiceDetailCellText(sa, column_id).compare(ServiceDetailCellText(sb, column_id));
	}
}

std::string EDIScopeGTK::ServiceDetailCellText(const LISTED_SERVICE& service, int column_id) const {
	std::string text;
	switch(column_id) {
	case 0:	// SubChId
		text = std::to_string(service.subchid);
		break;
	case 1:	// SID (without the "0x" prefix, to save space in the table) - unknown for a plain data sub-channel
		if(service.sid != LISTED_SERVICE::sid_none)
			text = StringTools::SidToHex(service.sid);
		break;
	case 2:	// ECC (without the "0x" prefix), only shown if it differs from the ensemble's own (FIG 0/9 exception)
		if(service.ecc != LISTED_SERVICE::ecc_none)
			text = StringTools::IntToHex(service.ecc, 2).substr(2);
		break;
	case 3:	{ // Label
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
	case 4:	{ // Short label
		std::string label = FICDecoder::ConvertLabelToUTF8(service.label, nullptr);
		text = FICDecoder::DeriveShortLabelUTF8(label, service.label.short_label_mask);
		break; }
	case 5:	// CUs
		if(!service.subchannel.IsNone() && !service.subchannel.pl.empty())
			text = std::to_string(service.subchannel.start) + "-" + std::to_string(service.subchannel.start + service.subchannel.size) +
					" (" + std::to_string(service.subchannel.size) + ")";
		break;
	case 6:	// Protection
		text = service.subchannel.pl;
		break;
	case 7:	// Bit rate
		if(!service.subchannel.IsNone() && !service.subchannel.pl.empty())
			text = std::to_string(service.subchannel.bitrate) + " kBit/s";
		break;
	case 8:	// Format (only known for the currently playing sub-channel, while it is being decoded)
		if(service.is_audio && service.audio_service.subchid == current_audio_format_subchid)
			text = current_audio_format.codec;
		break;
	case 9:	// Samplerate (ditto)
		if(service.is_audio && service.audio_service.subchid == current_audio_format_subchid && current_audio_format.samplerate_khz)
			text = std::to_string(current_audio_format.samplerate_khz) + " kHz";
		break;
	case 10:	// Audio mode (ditto)
		if(service.is_audio && service.audio_service.subchid == current_audio_format_subchid)
			text = current_audio_format.mode;
		break;
	case 11:	// Language
		if(service.subchannel.language != FIC_SUBCHANNEL::language_none)
			text = FICDecoder::ConvertLanguageToString(service.subchannel.language);
		break;
	case 12:	{ // PTY
		if(ensemble.inter_table_id != FIC_ENSEMBLE::inter_table_id_none) {
			int pty = service.pty_dynamic != LISTED_SERVICE::pty_none ? service.pty_dynamic : service.pty_static;
			if(pty != LISTED_SERVICE::pty_none)
				text = FICDecoder::ConvertPTYToString(pty, ensemble.inter_table_id);
		}
		break; }
	case 13:	// S - SPI Service Information present for this SId
		if(service.sid != LISTED_SERVICE::sid_none) {
			for(const SPI_SERVICE& spi_service : spi_info.services) {
				if(spi_service.has_dab_bearer && spi_service.sid == (uint32_t) service.sid) {
					text = "\xE2\x9C\x93";	// U+2713 CHECK MARK
					break;
				}
			}
		}
		break;
	case 14:	// P - at least one day's SPI Programme Information present for this SId
		if(service.sid != LISTED_SERVICE::sid_none) {
			for(const std::pair<const std::pair<uint32_t,std::string>,SPI_SCHEDULE>& kv : spi_schedules) {
				if(kv.first.first == (uint32_t) service.sid) {
					text = "\xE2\x9C\x93";
					break;
				}
			}
		}
		break;
	case 15: {	// R - RadioDNS (ETSI TS 103 270) resolution outcome, if ever tested - a checkmark
				// only when fully resolved; nothing otherwise (not yet tested and actively failed
				// look the same here on purpose, that distinction is what the rDNS window is for)
		if(service.sid != LISTED_SERVICE::sid_none) {
			int scids = service.IsPrimary() ? 0 : service.scids;
			std::map<std::pair<int,int>,RDNS_RESULT>::const_iterator it = rdns_results.find(std::make_pair(service.sid, scids));
			if(it != rdns_results.end() && it->second.FullyResolved())
				text = "\xE2\x9C\x93";	// U+2713 CHECK MARK
		}
		break; }
	}

	return text;
}

void EDIScopeGTK::RenderServiceDetailCell(Gtk::CellRenderer* renderer, const Gtk::TreeModel::iterator& it, int column_id) {
	Gtk::CellRendererText *text_renderer = static_cast<Gtk::CellRendererText*>(renderer);
	const LISTED_SERVICE& service = (LISTED_SERVICE) (*it)[services_table_cols.col_service];

	std::string text = ServiceDetailCellText(service, column_id);

	text_renderer->property_text() = text;
	if(column_id == 13 || column_id == 14 || column_id == 15)
		text_renderer->property_xalign() = 0.5;

	// not a playable service - de-emphasize the whole row instead of just leaving cells blank
	text_renderer->property_style() = service.is_audio ? Pango::STYLE_NORMAL : Pango::STYLE_ITALIC;

	std::string foreground;
	if(!service.is_audio)
		foreground = "grey";
	else if(column_id == 15 && !text.empty())
		foreground = "#2a8f3f";
	text_renderer->property_foreground_set() = !foreground.empty();
	if(!foreground.empty())
		text_renderer->property_foreground() = foreground;
}

bool EDIScopeGTK::on_services_table_query_tooltip(int x, int y, bool keyboard_tooltip, const Glib::RefPtr<Gtk::Tooltip>& tooltip) {
	if(keyboard_tooltip)
		return false;

	// query-tooltip reports widget coordinates (including the header row), while
	// get_path_at_pos expects bin window coordinates - they must be converted first
	int bin_x, bin_y;
	services_table.convert_widget_to_bin_window_coords(x, y, bin_x, bin_y);

	Gtk::TreeModel::Path path;
	Gtk::TreeViewColumn *column;
	int cell_x, cell_y;
	if(!services_table.get_path_at_pos(bin_x, bin_y, path, column, cell_x, cell_y))
		return false;

	Gtk::TreeModel::iterator it = services_table_liststore->get_iter(path);
	if(!it)
		return false;
	const LISTED_SERVICE& service = (LISTED_SERVICE) (*it)[services_table_cols.col_service];

	// row-wide details - more can be added here later
	if(service.sid == LISTED_SERVICE::sid_none)
		return false;
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

	tooltip->set_text(text);
	services_table.set_tooltip_row(tooltip, path);
	return true;
}

Gtk::ListStore::iterator EDIScopeGTK::UpdateServicesTableRow(const LISTED_SERVICE& service, bool* was_new) {
	Gtk::ListStore::Children children = services_table_liststore->children();
	Gtk::ListStore::iterator row_it = std::find_if(
			children.begin(), children.end(),
			[&](const Gtk::TreeModel::Row& row)->bool {
				const LISTED_SERVICE& ls = (LISTED_SERVICE) row[services_table_cols.col_service];
				return ls.sid == service.sid && ls.scids == service.scids;
			}
	);
	bool add_new_row = row_it == children.end();
	if(add_new_row)
		row_it = services_table_liststore->append();

	Gtk::TreeModel::Row row = *row_it;
	row[services_table_cols.col_service] = service;

	if(was_new)
		*was_new = add_new_row;
	return row_it;
}

void EDIScopeGTK::RebuildDataSubchannelRows() {
	// sub-channels already represented by a playable (audio) row
	std::set<int> claimed_subchids;
	for(const Gtk::TreeModel::Row& row : services_table_liststore->children()) {
		const LISTED_SERVICE& ls = (LISTED_SERVICE) row[services_table_cols.col_service];
		if(ls.is_audio)
			claimed_subchids.insert(ls.subchid);
	}

	// drop stale data-only rows (sub-channel gone, or now claimed by an audio service)
	Gtk::ListStore::Children children = services_table_liststore->children();
	for(Gtk::ListStore::iterator it = children.begin(); it != children.end();) {
		const LISTED_SERVICE& ls = (LISTED_SERVICE) (*it)[services_table_cols.col_service];
		if(!ls.is_audio && (known_subchannels.find(ls.subchid) == known_subchannels.end() || claimed_subchids.find(ls.subchid) != claimed_subchids.end()))
			it = services_table_liststore->erase(it);
		else
			++it;
	}

	// add/update a non-playable placeholder row for every remaining (data-only) sub-channel,
	// so it's listed - and its CUs accounted for - even though ediscope can't play it
	for(const std::pair<const int, FIC_SUBCHANNEL>& sc : known_subchannels) {
		if(claimed_subchids.find(sc.first) != claimed_subchids.end() || sc.second.IsNone() || sc.second.pl.empty())
			continue;

		children = services_table_liststore->children();
		Gtk::ListStore::iterator row_it = std::find_if(
				children.begin(), children.end(),
				[&](const Gtk::TreeModel::Row& row)->bool {
					const LISTED_SERVICE& ls = (LISTED_SERVICE) row[services_table_cols.col_service];
					return !ls.is_audio && ls.subchid == sc.first;
				}
		);
		if(row_it == children.end())
			row_it = services_table_liststore->append();

		LISTED_SERVICE ls;
		ls.is_audio = false;
		ls.subchid = sc.first;
		ls.pri_comp_subchid = sc.first;
		ls.subchannel = sc.second;
		std::map<int,uint32_t>::const_iterator sid_it = known_data_service_sids.find(sc.first);
		if(sid_it != known_data_service_sids.end())
			ls.sid = (int) sid_it->second;

		Gtk::TreeModel::Row row = *row_it;
		row[services_table_cols.col_service] = ls;
	}
}

void EDIScopeGTK::InitWidgets() {
	this->signal_delete_event().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_window_delete_event));

	// init widgets

	frame_source.set_label("Source");
	frame_source.add(network_box);

	network_box.set_spacing(WIDGET_SPACE);
	network_box.pack_start(combo_network_uri);
	network_box.pack_start(tglbtn_network_connect, Gtk::PACK_SHRINK);

	combo_network_uri.get_entry()->set_width_chars(22);
	combo_network_uri.get_entry()->set_placeholder_text("tcp://host:port");
	combo_network_uri.set_tooltip_text(
			"EDI network source\n"
			"tcp://host:port\n"
			"udp://host:port (unicast)\n"
			"udp://mcast-ip:port (multicast)\n"
			"\n"
			"Local file source\n"
			"file:///path/to/file.eti (or .edi)\n"
			"...or pick \"" + NETWORK_URI_BROWSE_ITEM + "\" from the dropdown"
	);
	for(const std::string& uri : LoadNetworkHistory())
		combo_network_uri.append(uri);
	combo_network_uri.prepend(NETWORK_URI_BROWSE_ITEM);
	combo_network_uri.signal_changed().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_combo_network_uri_changed));
	combo_network_uri.get_entry()->signal_activate().connect([this]() {
		if(!tglbtn_network_connect.get_active())
			tglbtn_network_connect.set_active(true);
	});

	tglbtn_network_connect.set_label("Connect");
	tglbtn_network_connect.signal_toggled().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_tglbtn_network_connect));

	frame_label_ensemble.set_label("Ensemble");
	frame_label_ensemble.add(label_ensemble);
	label_ensemble.set_halign(Gtk::ALIGN_START);
	label_ensemble.set_padding(WIDGET_SPACE, WIDGET_SPACE);
	// fixed width for "label (short_label) - EId": 16 + " (" + 8 + ")" + " - " + 4 chars, plus a cosmetic margin
	label_ensemble.set_width_chars(16 + 2 + 8 + 1 + 3 + 4 + 4);


	frame_services_table.set_label("Services details");
	frame_services_table.set_hexpand(true);

	services_table_liststore = Gtk::ListStore::create(services_table_cols);
	services_table_liststore->set_default_sort_func(sigc::mem_fun(*this, &EDIScopeGTK::ServicesTableSlotCompare));
	services_table_liststore->set_sort_column(Gtk::TreeSortable::DEFAULT_SORT_COLUMN_ID, Gtk::SORT_ASCENDING);

	services_table.set_model(services_table_liststore);
	services_table.set_headers_visible(true);
	services_table.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_services_table_selection_changed));
	// data services (e.g. EPG) are listed for CU accounting only - not selectable/playable
	services_table.get_selection()->set_select_function([this](const Glib::RefPtr<Gtk::TreeModel>& model, const Gtk::TreeModel::Path& path, bool) {
		return ((LISTED_SERVICE) (*model->get_iter(path))[services_table_cols.col_service]).is_audio;
	});

	static const char* services_table_column_titles[] =     {"SubChId", "SID", "ECC", "Label", "Short label", "CUs", "Protection", "Bit rate", "Format", "Samplerate", "Audio", "Language", "PTY", "S", "P", "R"};
	static const int services_table_column_min_widths[] =   {      60,    50,     50,    100,            80,     90,          80,         70,      110,           80,      70,         90,  180,   30,  30,  30};
	int services_table_min_width = 0;
	for(size_t i = 0; i < sizeof(services_table_column_titles) / sizeof(services_table_column_titles[0]); i++) {
		Gtk::CellRendererText *renderer = Gtk::manage(new Gtk::CellRendererText());
		int col_index = services_table.append_column(services_table_column_titles[i], *renderer) - 1;
		Gtk::TreeViewColumn *col = services_table.get_column(col_index);
		col->set_cell_data_func(*renderer, sigc::bind(sigc::mem_fun(*this, &EDIScopeGTK::RenderServiceDetailCell), (int) i));
		col->set_resizable(true);
		col->set_min_width(services_table_column_min_widths[i]);
		col->set_expand(false);	// otherwise GTK lets the last column (P) absorb all leftover width
		services_table_min_width += services_table_column_min_widths[i];

		// widest PTY string ("Children's programmes"/"Soft Rhythm and Blues") is 21 chars, plus a cosmetic margin
		if(std::string(services_table_column_titles[i]) == "PTY")
			renderer->property_width_chars() = 21 + 3;

		// "S"/"P"/"R" (single checkmark/cross glyph each) must match exactly - plain min_width
		// alone still lets GTK's default autosizing grow each one independently to its own
		// natural content width, which doesn't guarantee they end up equal
		std::string column_title = services_table_column_titles[i];
		if(column_title == "S" || column_title == "P" || column_title == "R") {
			col->set_sizing(Gtk::TREE_VIEW_COLUMN_FIXED);
			col->set_fixed_width(services_table_column_min_widths[i]);

			// a column header has no tooltip of its own - give it one via a custom header widget
			// (a plain label standing in for the title GTK would otherwise have drawn itself)
			static const std::map<std::string,std::string> column_tooltips = {
				{"S", "SPI Service Informations"},
				{"P", "SPI Programme Informations"},
				{"R", "RadioDNS"},
			};
			Gtk::Label *header_label = Gtk::manage(new Gtk::Label(column_title));
			header_label->set_tooltip_text(column_tooltips.at(column_title));
			header_label->show();
			col->set_widget(*header_label);
		} else {
			// every other column is orderable by clicking its header - S/P/R are deliberately
			// excluded above, they're just presence checkmarks, not a value worth sorting by
			services_table_liststore->set_sort_func((int) i, sigc::bind<0>(sigc::mem_fun(*this, &EDIScopeGTK::ServicesTableColumnCompare), (int) i));
			col->set_sort_column((int) i);
		}
	}

	// trailing dummy column, title-less and empty: absorbs whatever leftover width GTK would
	// otherwise hand to the last real column ("R") despite its own set_expand(false), so S/P/R
	// stay exactly as wide as set_fixed_width() says regardless of how wide the view ends up being
	{
		Gtk::CellRendererText *renderer = Gtk::manage(new Gtk::CellRendererText());
		int col_index = services_table.append_column("", *renderer) - 1;
		Gtk::TreeViewColumn *col = services_table.get_column(col_index);
		col->set_resizable(false);
		col->set_expand(true);
	}

	services_table.set_has_tooltip(true);
	services_table.signal_query_tooltip().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_services_table_query_tooltip));

	// both policies are ALWAYS, not AUTOMATIC: reserving both scrollbars' space up front means
	// this area's size never changes as rows/selection change (a scrollbar popping in/out, e.g.
	// once a row is selected and scrolled into view, otherwise changes how much height is left
	// for rows, which can cross the threshold for the OTHER scrollbar too) - otherwise, for the
	// instant before that settles, this area is briefly shorter than its steady-state height, and
	// the Stereo Meter/Correlation/Spectrum/Level/Slideshow row below picks up that slack
	services_table_scroll.set_policy(Gtk::POLICY_ALWAYS, Gtk::POLICY_ALWAYS);
	services_table_scroll.set_vexpand(true);
	services_table_scroll.add(services_table);
	// reserve enough room for all columns (unscrolled) and 15 services (scrollable beyond that)
	services_table_scroll.set_min_content_width(services_table_min_width + 40);	// + scrollbar/border margin
	services_table_scroll.set_min_content_height(15 * 26 + 30);	// ~26px/row + header
	// same as min: a secondary-component row can get added to the table at any time, as its FIC
	// data trickles in - without a matching max, that grows this area's natural height, which (at
	// a fixed window height) comes straight out of the Stereo Meter/Correlation/Spectrum/Level/
	// Slideshow row below. More rows than fit just scroll, exactly like fewer rows than fit does
	services_table_scroll.set_max_content_height(15 * 26 + 30);
	services_table_scroll.set_propagate_natural_height(false);

	services_table_box.set_orientation(Gtk::ORIENTATION_VERTICAL);
	services_table_box.pack_start(services_table_scroll, true, true);
	services_table_box.pack_start(cu_bar, false, false);
	cu_bar.signal_service_clicked.connect(sigc::mem_fun(*this, &EDIScopeGTK::on_cu_bar_service_clicked));
	frame_services_table.add(services_table_box);
	frame_services_table.set_vexpand(true);

	tglbtn_record.set_image_from_icon_name("media-record");
	tglbtn_record.signal_clicked().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_tglbtn_record));
	tglbtn_record.set_sensitive(false);
	tglbtn_record.set_tooltip_text("Record service");
	tglbtn_record.set_halign(Gtk::ALIGN_START);
	tglbtn_record.set_margin_top(10);	// align with the frames to the left, whose label strip pushes their content down

	label_record.set_halign(Gtk::ALIGN_START);
	label_record.set_padding(WIDGET_SPACE, WIDGET_SPACE);

	tglbtn_logs.set_label("Logs");
	tglbtn_logs.signal_clicked().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_tglbtn_logs));
	tglbtn_logs.set_tooltip_text("Log of hot changes on the ensemble");
	tglbtn_logs.set_halign(Gtk::ALIGN_START);
	tglbtn_logs.set_margin_top(10);

	tglbtn_spi.set_label("SPI");
	tglbtn_spi.signal_clicked().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_tglbtn_spi));
	tglbtn_spi.set_tooltip_text("Service and Programme Information (ETSI TS 102 371/102 818), if the ensemble carries any");
	tglbtn_spi.set_halign(Gtk::ALIGN_START);
	tglbtn_spi.set_margin_top(10);

	tglbtn_rdns.set_label("rDNS");
	tglbtn_rdns.signal_clicked().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_tglbtn_rdns));
	tglbtn_rdns.set_tooltip_text("RadioDNS (ETSI TS 103 270) resolution for the ensemble's services - trace where it works/blocks");
	tglbtn_rdns.set_halign(Gtk::ALIGN_START);
	tglbtn_rdns.set_margin_top(10);

	tglbtn_links.set_label("Links");
	tglbtn_links.signal_clicked().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_tglbtn_links));
	tglbtn_links.set_tooltip_text("Service linking and Frequency Information (ETSI EN 300 401 clauses 8.1.8/8.1.15), if the ensemble carries any");
	tglbtn_links.set_halign(Gtk::ALIGN_START);
	tglbtn_links.set_margin_top(10);

	tglbtn_mute.set_image_from_icon_name("audio-volume-muted");
	tglbtn_mute.signal_clicked().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_tglbtn_mute));
	tglbtn_mute.set_tooltip_text("Mute");
	tglbtn_mute.set_halign(Gtk::ALIGN_START);
	tglbtn_mute.set_margin_top(10);

	vlmbtn.set_value(1.0);
	vlmbtn.set_sensitive(ensemble_player->HasAudioVolumeControl());
	vlmbtn.signal_value_changed().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_vlmbtn));
	vlmbtn.set_halign(Gtk::ALIGN_START);
	vlmbtn.set_margin_top(10);

	frame_label_dl.set_label("Dynamic Label");
	frame_label_dl.set_size_request(-1, 50);
	frame_label_dl.set_sensitive(false);
	frame_label_dl.add(label_dl);
	label_dl.set_halign(Gtk::ALIGN_START);
	label_dl.set_valign(Gtk::ALIGN_START);
	label_dl.set_padding(WIDGET_SPACE, WIDGET_SPACE);
	label_dl.set_selectable(true);	// enables text selection + the native right-click "Copy" menu

	frame_dl_plus.set_label("Dynamic Label Plus");
	// always show 2 rows' worth of height, even with 0 or 1 actual rows; a 3rd+ DL Plus object
	// scrolls instead of growing the frame further
	dl_plus_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
	dl_plus_scroll.set_min_content_height(2 * 26 + 26);	// ~26px/row + header
	dl_plus_scroll.set_max_content_height(2 * 26 + 26);
	dl_plus_scroll.add(dl_plus_content);
	frame_dl_plus.add(dl_plus_scroll);

	label_dl_row.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
	label_dl_row.set_spacing(WIDGET_SPACE);	// same gap as between any other two boxes
	// Dynamic Label is fixed-width (see dl_width_group below); DL Plus takes whatever remains
	label_dl_row.pack_start(frame_label_dl, false, false);
	label_dl_row.pack_start(frame_dl_plus, true, true);

	frame_stereo_meter.set_label("Stereo Meter");
	frame_stereo_meter.add(stereo_meter);

	frame_correlation.set_label("Correlation");
	frame_correlation.add(correlation_meter);

	stereo_correlation_box.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
	stereo_correlation_box.set_spacing(WIDGET_SPACE);
	stereo_correlation_box.pack_start(frame_stereo_meter, false, false);
	stereo_correlation_box.pack_start(frame_correlation, false, false);

	frame_spectrum.set_label("Spectrum");
	frame_spectrum.add(spectrum);

	frame_level_meter.set_label("Level");
	frame_level_meter.add(level_meter);

	frame_slideshow.set_label("Slideshow");
	frame_slideshow.add(slideshow_content);

	// grouped below with Dynamic Label, so its right edge lines up with Spectrum's right edge
	meters_left_box.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
	meters_left_box.set_spacing(WIDGET_SPACE);
	meters_left_box.pack_start(stereo_correlation_box, false, false);
	meters_left_box.pack_start(frame_spectrum, false, false);

	meters_slideshow_box.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
	meters_slideshow_box.set_spacing(WIDGET_SPACE);
	meters_slideshow_box.pack_start(meters_left_box, false, false);
	meters_slideshow_box.pack_start(frame_level_meter, false, false);
	meters_slideshow_box.pack_start(frame_slideshow, false, false);

	// forces Dynamic Label to exactly match the combined width of Stereo Meter + Correlation +
	// Spectrum, so its right edge lines up with Spectrum's right edge
	dl_width_group = Gtk::SizeGroup::create(Gtk::SIZE_GROUP_HORIZONTAL);
	dl_width_group->add_widget(frame_label_dl);
	dl_width_group->add_widget(meters_left_box);

	lower_controls_box.set_orientation(Gtk::ORIENTATION_VERTICAL);
	lower_controls_box.set_spacing(WIDGET_SPACE);
	lower_controls_box.pack_start(label_dl_row, false, false);
	lower_controls_box.pack_start(meters_slideshow_box, false, false);
	// the level meter needs vexpand to fill its own frame, but that must not bubble up into
	// top_grid's row allocation - otherwise enlarging the window vertically splits the extra
	// space between "Services details" and this row, instead of only growing the former
	lower_controls_box.set_vexpand(false);

	frame_label_datetime.set_label("Local date/time");
	frame_label_datetime.set_sensitive(false);
	frame_label_datetime.add(label_datetime);
	label_datetime.set_halign(Gtk::ALIGN_START);
	label_datetime.set_padding(WIDGET_SPACE, WIDGET_SPACE);
	// fixed width for "YYYY-MM-DD, Www - HH:MM:SS": 10 + 2 + 3 + 3 + 8 chars, plus a cosmetic margin
	label_datetime.set_width_chars(10 + 2 + 3 + 3 + 8 + 4);

	frame_label_edi_delay.set_label("Margin delay");
	frame_label_edi_delay.set_sensitive(false);
	frame_label_edi_delay.add(label_edi_delay);
	frame_label_edi_delay.set_has_tooltip(true);
	frame_label_edi_delay.signal_query_tooltip().connect(sigc::mem_fun(*this, &EDIScopeGTK::on_edi_delay_query_tooltip));
	label_edi_delay.set_halign(Gtk::ALIGN_START);
	label_edi_delay.set_padding(WIDGET_SPACE, WIDGET_SPACE);
	label_edi_delay.set_width_chars(10);	// "-1234 ms"

	frame_label_asu.set_label("Announcement support");
	frame_label_asu.set_sensitive(false);
	frame_label_asu.set_hexpand(true);
	frame_label_asu.add(label_asu);
	label_asu.set_halign(Gtk::ALIGN_START);
	label_asu.set_padding(WIDGET_SPACE, WIDGET_SPACE);

	progress_position.set_show_text();


	top_grid.set_column_spacing(WIDGET_SPACE);
	top_grid.set_row_spacing(WIDGET_SPACE);

	// kept in its own grid (not sharing columns with the much wider services table below), so growing
	// the window never redistributes extra width onto its columns and makes the buttons drift apart
	header_grid.set_column_spacing(WIDGET_SPACE);
	header_grid.set_row_spacing(WIDGET_SPACE);
	header_grid.set_halign(Gtk::ALIGN_START);

	header_grid.attach(frame_source, 0, 0, 1, 2);
	header_grid.attach_next_to(frame_label_ensemble, frame_source, Gtk::POS_RIGHT, 1, 2);
	header_grid.attach_next_to(frame_label_datetime, frame_label_ensemble, Gtk::POS_RIGHT, 1, 2);
	header_grid.attach_next_to(frame_label_edi_delay, frame_label_datetime, Gtk::POS_RIGHT, 1, 2);
	header_grid.attach_next_to(tglbtn_logs, frame_label_edi_delay, Gtk::POS_RIGHT, 1, 1);
	header_grid.attach_next_to(tglbtn_spi, tglbtn_logs, Gtk::POS_RIGHT, 1, 1);
	header_grid.attach_next_to(tglbtn_rdns, tglbtn_spi, Gtk::POS_RIGHT, 1, 1);
	header_grid.attach_next_to(tglbtn_links, tglbtn_rdns, Gtk::POS_RIGHT, 1, 1);
	header_grid.attach_next_to(tglbtn_record, tglbtn_links, Gtk::POS_RIGHT, 1, 1);
	header_grid.attach_next_to(label_record, tglbtn_record, Gtk::POS_RIGHT, 3, 1);
	header_grid.attach_next_to(tglbtn_mute, label_record, Gtk::POS_RIGHT, 1, 1);
	header_grid.attach_next_to(vlmbtn, tglbtn_mute, Gtk::POS_RIGHT, 1, 1);

	// add widgets
	add(top_grid);

	top_grid.attach(header_grid, 0, 0, 1, 1);
	top_grid.attach_next_to(frame_services_table, header_grid, Gtk::POS_BOTTOM, 1, 1);
	top_grid.attach_next_to(lower_controls_box, frame_services_table, Gtk::POS_BOTTOM, 1, 1);
	top_grid.attach_next_to(frame_label_asu, lower_controls_box, Gtk::POS_BOTTOM, 1, 1);
	top_grid.attach_next_to(progress_position, frame_label_asu, Gtk::POS_BOTTOM, 1, 1);

	show_all_children();
	progress_position.hide();	// invisible until progress updated
}

void EDIScopeGTK::SetService(const LISTED_SERVICE& service) {
	// (re)set labels/tooltips/record button
	if(!service.IsNone()) {
		std::string label = FICDecoder::ConvertLabelToUTF8(service.label, nullptr);

		set_title(label + " - EDIScope");

		tglbtn_record.set_sensitive(true);
	} else {
		set_title("EDIScope");

		tglbtn_record.set_sensitive(false);
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
		services_table.queue_draw();

		frame_label_dl.set_sensitive(false);
		label_dl.set_label("");
		frame_label_dl.set_tooltip_text("");

		dl_plus_content.ClearDLPlusInfo();
		level_meter.Reset();
		spectrum.Reset();
		stereo_meter.Reset();
		correlation_meter.Reset();

		if(!service.HasSLS())
			slideshow_content.ClearSlide();

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
	if(service.HasSLS() && (audio_service_changed || slideshow_content.IsEmptySlide())) {
		slideshow_content.AwaitSlide();
		pad_decoder->SetMOTAppType(service.sls_app_type);
	}
}

void EDIScopeGTK::UpdateAnnouncementSupport(const LISTED_SERVICE& service) {
	// reset, if no service/announcement support
	if(service.IsNone() || service.asu_flags == LISTED_SERVICE::asu_flags_none) {
		frame_label_asu.set_sensitive(false);
		frame_label_asu.set_tooltip_markup("");
		label_asu.set_label("");
		return;
	}

	// assemble text - iterate through all supported announcement types
	std::string ac_str;
	size_t count = 0;
	for(int type = 0; type < 16; type++) {
		uint16_t type_flag = 1 << type;
		if(!(service.asu_flags & type_flag))
			continue;

		// check all matching announcement clusters for active announcement
		std::string asw_color;
		for(const cids_t::value_type& cid : service.cids) {
			asw_clusters_t::const_iterator it = ensemble.asw_clusters.find(cid);
			if(it == ensemble.asw_clusters.cend() || !(it->second.asw_flags & type_flag))
				continue;

			if(it->second.subchid == service.audio_service.subchid) {
				asw_color = "yellow";
			} else {
				if(asw_color.empty())
					asw_color = "cyan";
			}
		}

		// regularly insert line break
		if(!ac_str.empty()) {
			ac_str += (count % 6 == 0) ? "\n" : " ";
			ac_str += "+ ";
		}
		count++;

		if(!asw_color.empty())
			ac_str += "<span bgcolor='" + asw_color + "'>";
		ac_str += FICDecoder::ConvertASuTypeToString(type);
		if(!asw_color.empty())
			ac_str += "</span>";
	}

	// assemble tooltip - iterate through all matching announcement clusters
	std::string ac_tooltip;
	char cid_string[5];
	for(const cids_t::value_type& cid : service.cids) {
		asw_clusters_t::const_iterator it = ensemble.asw_clusters.find(cid);
		bool present = it != ensemble.asw_clusters.cend();
		bool active = present && (service.asu_flags & it->second.asw_flags);

		ac_tooltip += ac_tooltip.empty() ? "Cluster(s): " : " / ";
		if(active)
			ac_tooltip += "<u>";
		snprintf(cid_string, sizeof(cid_string), "0x%02X", cid);
		ac_tooltip += std::string(cid_string);
		if(present)
			ac_tooltip += " (SubChId " + std::to_string(it->second.subchid) + ")";
		if(active)
			ac_tooltip += "</u>";
	}

	frame_label_asu.set_sensitive(true);
	frame_label_asu.set_tooltip_markup(ac_tooltip);
	label_asu.set_markup(ac_str);
}

void EDIScopeGTK::on_tglbtn_record() {
	if(tglbtn_record.get_active()) {
		// get start timestamp
		time_t start;
		{
			std::lock_guard<std::mutex> lock(rec_mutex);

			// use prebuffer start information, if available
			if(!rec_prebuffer.empty()) {
				start = rec_prebuffer.front().ts;
			} else {
				start = time(nullptr);
				if(start == (time_t) -1)
					perror("EDIScopeGTK: error while getting time for recording");
			}
		}
		struct tm* start_tm = localtime(&start);
		if(!start_tm)
			perror("EDIScopeGTK: error while getting local time");
		char start_string[22];
		strftime(start_string, sizeof(start_string), "%F - %H-%M-%S", start_tm);

		// get station name
		LISTED_SERVICE service;	// default: none
		Gtk::TreeModel::iterator row_it = services_table.get_selection()->get_selected();
		if(row_it)
			service = (LISTED_SERVICE) (*row_it)[services_table_cols.col_service];
		std::string label = FICDecoder::ConvertLabelToUTF8(service.label, nullptr);

		// escape forbidden '/' character
		std::string label_cleaned = label;
		for(char& c : label_cleaned)
			if(c == '/')
				c = '_';

		std::string new_rec_filename = options.recordings_path + "/" + std::string(start_string) + " - " + label_cleaned + "." + ensemble_player->GetUntouchedStreamFileExtension();
		FILE* new_rec_file = fopen(new_rec_filename.c_str(), "wb");
		if(new_rec_file) {
			// disable service switch
			services_table.set_sensitive(false);
			cu_bar.set_sensitive(false);

			fprintf(stderr, "EDIScopeGTK: recording started into '%s'\n", new_rec_filename.c_str());

			{
				std::lock_guard<std::mutex> lock(rec_mutex);

				rec_file = new_rec_file;
				rec_filename = new_rec_filename;

				// write prebuffer
				for(const RecSample& s : rec_prebuffer)
					if(fwrite(&s.data[0], s.data.size(), 1, rec_file) != 1)
						perror("EDIScopeGTK: error while writing untouched stream prebuffer to file");
				rec_duration_ms = rec_prebuffer_filled_ms;

				// clear prebuffer
				rec_prebuffer.clear();
				rec_prebuffer_filled_ms = 0;

				UpdateRecStatus(true);
			}

			if(options.rec_prebuffer_size_s == 0)
				ensemble_player->AddUntouchedStreamConsumer(this);
		} else {
			int fopen_errno = errno;	// save before overwrite
			tglbtn_record.set_active(false);

			Gtk::MessageDialog hint(*this, "Could not start recording!", false, Gtk::MESSAGE_ERROR);
			hint.set_title("EDIScope");
			hint.set_secondary_text(strerror(fopen_errno), false);
			hint.run();
		}
	} else {
		std::lock_guard<std::mutex> lock(rec_mutex);

		// only stop recording, if active (handles error on recording start)
		if(rec_file) {
			if(options.rec_prebuffer_size_s == 0)
				ensemble_player->RemoveUntouchedStreamConsumer(this);

			fclose(rec_file);
			rec_file = nullptr;

			fprintf(stderr, "EDIScopeGTK: recording stopped\n");
			UpdateRecStatus(true);

			// enable service switch
			services_table.set_sensitive(true);
			cu_bar.set_sensitive(true);
		}
	}
}

void EDIScopeGTK::ProcessUntouchedStream(const uint8_t* data, size_t len, size_t duration_ms) {
	std::lock_guard<std::mutex> lock(rec_mutex);

	// if recording in progress
	if(rec_file) {
		long int rec_duration_ms_old = rec_duration_ms;

		// write sample
		if(fwrite(data, len, 1, rec_file) != 1)
			perror("EDIScopeGTK: error while writing untouched stream sample to file");
		rec_duration_ms += duration_ms;

		// update status only on seconds change
		if(rec_duration_ms / 1000 != rec_duration_ms_old / 1000)
			DoRecStatusUpdate(true);
	} else {
		// if prebuffer enabled
		if(options.rec_prebuffer_size_s > 0) {
			long int rec_prebuffer_ms_old = rec_prebuffer_filled_ms;

			// append sample
			rec_prebuffer.emplace_back(data, len, duration_ms);
			rec_prebuffer_filled_ms += duration_ms;

			// remove samples while needed
			while(rec_prebuffer_filled_ms - rec_prebuffer.front().duration_ms >= options.rec_prebuffer_size_s * 1000) {
				rec_prebuffer_filled_ms -= rec_prebuffer.front().duration_ms;
				rec_prebuffer.pop_front();
			}

			// update status only on seconds change
			if(rec_prebuffer_filled_ms / 1000 != rec_prebuffer_ms_old / 1000)
				DoRecStatusUpdate(true);
		}
	}
}

void EDIScopeGTK::DoRecStatusUpdateEmitted() {
	std::lock_guard<std::mutex> lock(rec_mutex);
	UpdateRecStatus(do_rec_status_update.Pop());
}

void EDIScopeGTK::UpdateRecStatus(bool decoding) {
	// mutex must already be locked!

	// if recording in progress
	if(rec_file) {
		label_record.set_label(StringTools::MsToTimecode(rec_duration_ms));
		label_record.set_sensitive(true);
		label_record.set_tooltip_text("File name: \"" + rec_filename + "\"");
	} else {
		// if prebuffer enabled and currently decoding
		if(options.rec_prebuffer_size_s > 0 && decoding) {
			label_record.set_label(StringTools::MsToTimecode(rec_prebuffer_filled_ms));
			label_record.set_sensitive(false);
			label_record.set_tooltip_text("Prebuffer: " + StringTools::MsToTimecode(options.rec_prebuffer_size_s * 1000));
		} else {
			label_record.set_label("");
			label_record.set_tooltip_text("");
		}
	}
}


void EDIScopeGTK::on_tglbtn_mute() {
	ensemble_player->SetAudioMute(tglbtn_mute.get_active());
}

void EDIScopeGTK::on_vlmbtn(double value) {
	ensemble_player->SetAudioVolume(value);

	// disable mute, if needed
	if(tglbtn_mute.get_active())
		tglbtn_mute.clicked();
}

void EDIScopeGTK::on_tglbtn_logs() {
	if(tglbtn_logs.get_active()) {
		logs_window.AlignToParent();
		logs_window.show();
	} else {
		logs_window.hide();
	}
}

void EDIScopeGTK::on_tglbtn_spi() {
	if(tglbtn_spi.get_active()) {
		spi_window.AlignToParent();
		spi_window.SetKnownServiceNames(known_service_names);
		spi_window.Update(spi_info, spi_logo_data);
		for(const std::pair<const std::pair<uint32_t,std::string>,SPI_SCHEDULE>& kv : spi_schedules)
			spi_window.UpdateSchedule(kv.second);
		spi_window.UpdateGroupInformation(spi_group_info, spi_logo_data);
		spi_window.show();
	} else {
		spi_window.hide();
	}
}

void EDIScopeGTK::on_tglbtn_rdns() {
	if(tglbtn_rdns.get_active()) {
		rdns_window.AlignToParent();
		rdns_window.SetServices(BuildRDNSServiceList());
		// services are tested automatically as soon as they're known (see MaybeStartRDNSTest()),
		// independently of this window ever being open - replay whatever was already found out
		// about them in the meantime, instead of showing blank statuses until the next hot change
		for(const std::pair<const std::pair<int,int>,RDNS_RESULT>& kv : rdns_results)
			rdns_window.UpdateResult(kv.first.first, kv.first.second, kv.second);
		rdns_window.show();
	} else {
		rdns_window.hide();
	}
}

void EDIScopeGTK::on_tglbtn_links() {
	if(tglbtn_links.get_active()) {
		links_window.AlignToParent();
		links_window.SetEnsembleEid(ensemble.eid);
		links_window.SetServices(BuildLinksServiceList());
		// Service linking sets and Frequency Information both get decoded as soon as they're seen
		// on the FIC, independently of this window ever being open - replay whatever is already
		// known instead of showing nothing until the next hot change
		for(const fic_service_linking_sets_t::value_type& kv : service_linking_sets)
			links_window.UpdateServiceLinking(kv.first, kv.second);
		for(const fic_frequency_infos_t::value_type& kv : frequency_infos)
			links_window.UpdateFrequencyInfo(kv.first, kv.second);
		for(const fic_oe_services_t::value_type& kv : oe_services)
			links_window.UpdateOEService(kv.first, kv.second);
		links_window.show();
	} else {
		links_window.hide();
	}
}

bool EDIScopeGTK::on_window_delete_event(GdkEventAny* /*any_event*/) {
	// prevent exit while recording
	if(tglbtn_record.get_active()) {
		Gtk::MessageDialog hint(*this, "Cannot exit while recording!", false, Gtk::MESSAGE_ERROR);
		hint.set_title("EDIScope");
		hint.set_secondary_text("The recording has to be stopped first.", false);
		hint.run();
		return true;
	} else {
		return false;
	}
}

bool EDIScopeGTK::HandleKeyPressEvent(GdkEventKey* key_event) {
	// events without Shift/Control/Alt/Win
	if((key_event->state & (Gdk::SHIFT_MASK | Gdk::CONTROL_MASK | Gdk::MOD1_MASK | Gdk::SUPER_MASK)) == 0) {
		switch(key_event->keyval) {
		case GDK_KEY_m:
		case GDK_KEY_M:
			// toggle mute
			tglbtn_mute.clicked();
			return true;
		case GDK_KEY_r:
		case GDK_KEY_R:
			// toggle record, if allowed
			if(tglbtn_record.get_sensitive())
				tglbtn_record.clicked();
			return true;
		}

		// try to switch service (1 to 10, -/+)
		int new_index;
		if(CheckForIndexKey(key_event, GetSelectedServiceIndex(), new_index)) {
			TrySwitchService(new_index);
			return true;
		}
	}

	// events without Shift/Control/Win, but with Alt
	if((key_event->state & (Gdk::SHIFT_MASK | Gdk::CONTROL_MASK | Gdk::SUPER_MASK)) == 0 && (key_event->state & Gdk::MOD1_MASK)) {
		// try to switch service (11 to 20)
		int new_index;
		if(CheckForIndexKey(key_event, -1, new_index)) {
			TrySwitchService(new_index + 10);
			return true;
		}
	}

	// events without Shift/Alt/Win, but with Control
	if((key_event->state & (Gdk::SHIFT_MASK | Gdk::MOD1_MASK | Gdk::SUPER_MASK)) == 0 && (key_event->state & Gdk::CONTROL_MASK)) {
		switch(key_event->keyval) {
		case GDK_KEY_c:
		case GDK_KEY_C: {
			// copy DL to clipboard, if not empty
			std::string dl = label_dl.get_label();
			if(!dl.empty())
				Gtk::Clipboard::get()->set_text(dl);
			return true; }
		}
	}

	return false;
}

bool EDIScopeGTK::CheckForIndexKey(GdkEventKey* key_event, int old_index, int& new_index) {
	switch(key_event->keyval) {
	case GDK_KEY_1:
	case GDK_KEY_KP_1:
		new_index = 0;
		return true;
	case GDK_KEY_2:
	case GDK_KEY_KP_2:
		new_index = 1;
		return true;
	case GDK_KEY_3:
	case GDK_KEY_KP_3:
		new_index = 2;
		return true;
	case GDK_KEY_4:
	case GDK_KEY_KP_4:
		new_index = 3;
		return true;
	case GDK_KEY_5:
	case GDK_KEY_KP_5:
		new_index = 4;
		return true;
	case GDK_KEY_6:
	case GDK_KEY_KP_6:
		new_index = 5;
		return true;
	case GDK_KEY_7:
	case GDK_KEY_KP_7:
		new_index = 6;
		return true;
	case GDK_KEY_8:
	case GDK_KEY_KP_8:
		new_index = 7;
		return true;
	case GDK_KEY_9:
	case GDK_KEY_KP_9:
		new_index = 8;
		return true;
	case GDK_KEY_0:
	case GDK_KEY_KP_0:
		new_index = 9;
		return true;
	case GDK_KEY_minus:
	case GDK_KEY_KP_Subtract:
		if(old_index == -1)
			break;
		new_index = old_index - 1;
		return true;
	case GDK_KEY_plus:
	case GDK_KEY_KP_Add:
		if(old_index == -1)
			break;
		new_index = old_index + 1;
		return true;
	}
	return false;
}

int EDIScopeGTK::GetSelectedServiceIndex() {
	Gtk::TreeModel::iterator row_it = services_table.get_selection()->get_selected();
	if(!row_it)
		return -1;

	Gtk::TreeModel::Path path = services_table_liststore->get_path(row_it);
	return path.empty() ? -1 : path[0];
}

void EDIScopeGTK::TrySwitchService(int index) {
	// switch, if allowed and index valid
	if(!services_table.is_sensitive() || index < 0 || index >= (signed) services_table_liststore->children().size())
		return;

	Gtk::TreeModel::Path path;
	path.push_back(index);
	Gtk::TreeModel::iterator row_it = services_table_liststore->get_iter(path);
	if(row_it)
		services_table.get_selection()->select(row_it);
}

void EDIScopeGTK::UpdateCUBar() {
	std::vector<LISTED_SERVICE> services;
	for(const Gtk::TreeModel::Row& row : services_table_liststore->children())
		services.push_back((LISTED_SERVICE) row[services_table_cols.col_service]);
	cu_bar.SetServices(services);
	cu_bar.SetSubchannels(known_subchannels);

	int selected_subchid = AUDIO_SERVICE::subchid_none;
	Gtk::TreeModel::iterator current_it = services_table.get_selection()->get_selected();
	if(current_it)
		selected_subchid = ((LISTED_SERVICE) (*current_it)[services_table_cols.col_service]).audio_service.subchid;
	cu_bar.SetSelectedSubchId(selected_subchid);
}

void EDIScopeGTK::EnsembleUpdateProgressEmitted() {
//	fprintf(stderr, "### EnsembleUpdateProgressEmitted\n");

	ENSEMBLE_PROGRESS progress = ensemble_update_progress.Pop();

	progress_position.set_fraction(progress.value);
	progress_position.set_text(progress.text);
	if(!progress_position.get_visible())
		progress_position.show();
}

void EDIScopeGTK::EnsembleChangeFormatEmitted() {
//	fprintf(stderr, "### EnsembleChangeFormatEmitted\n");

	current_audio_format = ensemble_change_format.Pop();

	// remember which sub-channel this format applies to, so the services table can show it for that row only
	Gtk::TreeModel::iterator current_it = services_table.get_selection()->get_selected();
	current_audio_format_subchid = current_it ? ((LISTED_SERVICE) (*current_it)[services_table_cols.col_service]).audio_service.subchid : AUDIO_SERVICE::subchid_none;
	services_table.queue_draw();
}

void EDIScopeGTK::EnsembleAudioLevelEmitted() {
	level_meter.SetLevels(ensemble_audio_level.Pop());
}

void EDIScopeGTK::EnsembleAudioSpectrumEmitted() {
	spectrum.SetSpectrum(ensemble_audio_spectrum.Pop());
}

void EDIScopeGTK::EnsembleAudioStereoEmitted() {
	AUDIO_STEREO_FRAME stereo = ensemble_audio_stereo.Pop();
	stereo_meter.AddPoints(stereo.points);
	if(stereo.valid)
		correlation_meter.SetCorrelation(stereo.correlation);
}

void EDIScopeGTK::EnsembleEDITimingEmitted() {
	EDI_TIMING timing = ensemble_edi_timing.Pop();

	if(timing.valid) {
		frame_label_edi_delay.set_sensitive(true);
		label_edi_delay.set_label(std::to_string((int) std::lround(timing.margin_delay_ms)) + " ms");
	} else {
		frame_label_edi_delay.set_sensitive(false);
		label_edi_delay.set_label("");
	}
	edi_timing_valid = timing.valid;
	edi_timing_tist_at_fct0_ms = timing.tist_at_fct0_ms;
}

bool EDIScopeGTK::on_edi_delay_query_tooltip(int /*x*/, int /*y*/, bool keyboard_tooltip, const Glib::RefPtr<Gtk::Tooltip>& tooltip) {
	if(keyboard_tooltip || !edi_timing_valid)
		return false;

	tooltip->set_text("TIST@FCT0: " + std::to_string((int) std::lround(edi_timing_tist_at_fct0_ms)) + " ms");
	return true;
}

void EDIScopeGTK::FICChangeEnsemble(const FIC_ENSEMBLE& ensemble) {
	fic_change_ensemble.PushAndEmit(ensemble);

	// update LTO for date/time + show
	if(dt_lto != ensemble.lto) {
		dt_lto = ensemble.lto;
//		fprintf(stderr, "### LTO updated\n");
		ShowDateTime(false);
	}
}

void EDIScopeGTK::FICChangeEnsembleEmitted() {
//	fprintf(stderr, "### FICChangeEnsembleEmitted\n");

	ensemble = fic_change_ensemble.Pop();
	RescanRDNSServices();	// the ensemble's ECC (hence every service's GCC) may have just changed
	if(links_window.get_visible())
		links_window.SetEnsembleEid(ensemble.eid);

	std::string charset_name;
	std::string label_plain = FICDecoder::ConvertLabelToUTF8(ensemble.label, &charset_name);
	std::string short_label_plain = FICDecoder::DeriveShortLabelUTF8(label_plain, ensemble.label.short_label_mask);

	std::string tooltip_text =
			"Short label: \"" + short_label_plain + "\"\n"
			"Label charset: " + charset_name + "\n"
			"EId: " + StringTools::IntToHex(ensemble.eid, 4).substr(2) + "\n"
			"Alarm flag: " + (ensemble.al_flag ? "true" : "false");
	if(ensemble.ecc != FIC_ENSEMBLE::ecc_none) {
		tooltip_text += "\n" "ECC: " + StringTools::IntToHex(ensemble.ecc, 2).substr(2);
		const char *country = LookupECCCountry(ensemble.ecc, ensemble.eid >> 12);
		if(country)
			tooltip_text += " (" + std::string(country) + ")";
	}
	if(ensemble.lto != FIC_ENSEMBLE::lto_none)
		tooltip_text += "\n" "LTO: " + FICDecoder::ConvertLTOToString(ensemble.lto);
	if(ensemble.inter_table_id != FIC_ENSEMBLE::inter_table_id_none)
		tooltip_text += "\n" "International table ID: " + StringTools::IntToHex(ensemble.inter_table_id, 2) + " (" + FICDecoder::ConvertInterTableIDToString(ensemble.inter_table_id) + ")";

	// label (short label) - EId, without the "0x" prefix
	std::string label =
			Glib::Markup::escape_text(label_plain) +
			" (" + Glib::Markup::escape_text(short_label_plain) + ")" +
			" - " + StringTools::IntToHex(ensemble.eid, 4).substr(2);

	// indicate active alarm announcement
	if(ensemble.al_flag) {
		asw_clusters_t::const_iterator cl_it = ensemble.asw_clusters.find(0xFF);
		if(cl_it != ensemble.asw_clusters.end() && (cl_it->second.asw_flags & 0x01)) {
			label += " <span color='white' bgcolor='red'><b> Alarm </b></span>";
			tooltip_text += "\n" "Alarm SubChId: " + std::to_string(cl_it->second.subchid);
		}
	}

	label_ensemble.set_markup(label);
	frame_label_ensemble.set_tooltip_text(tooltip_text);
}

void EDIScopeGTK::FICChangeServiceEmitted() {
//	fprintf(stderr, "### FICChangeServiceEmitted\n");

	LISTED_SERVICE new_service = fic_change_service.Pop();
	std::string label = FICDecoder::ConvertLabelToUTF8(new_service.label, nullptr);

	if(!label.empty()) {
		known_service_names[(uint32_t) new_service.sid] = label;
		if(spi_window.get_visible())
			spi_window.SetKnownServiceNames(known_service_names);
	}

	bool add_new_row;
	Gtk::ListStore::iterator row_it = UpdateServicesTableRow(new_service, &add_new_row);

	if(add_new_row) {
		// if first found service requested, adopt service params (for possible later changes)
		if(options.initial_first_found_service) {
			switch_service_sid = new_service.sid;
			switch_service_scids = new_service.scids;
			options.initial_first_found_service = false;
		}

		// set (initial) service
		if(label == switch_service_label || (new_service.sid == switch_service_sid && new_service.scids == switch_service_scids))
			services_table.get_selection()->select(row_it);
	} else {
		// set (updated) service
		Gtk::TreeModel::iterator current_it = services_table.get_selection()->get_selected();
		if(current_it && current_it == row_it)
			SetService(new_service);
	}

	RebuildDataSubchannelRows();
	UpdateCUBar();

	if(new_service.is_audio) {
		int test_scids = new_service.IsPrimary() ? 0 : new_service.scids;
		int test_ecc = new_service.ecc != LISTED_SERVICE::ecc_none ? new_service.ecc : ensemble.ecc;
		MaybeStartRDNSTest(new_service.sid, test_scids, test_ecc, ensemble.eid);
	}

	if(rdns_window.get_visible())
		rdns_window.SetServices(BuildRDNSServiceList());
	if(links_window.get_visible())
		links_window.SetServices(BuildLinksServiceList());
}

void EDIScopeGTK::FICRemoveServiceEmitted() {
	fic_service_id_t id = fic_remove_service.Pop();

	Gtk::ListStore::Children children = services_table_liststore->children();
	Gtk::ListStore::iterator row_it = std::find_if(
			children.begin(), children.end(),
			[&](const Gtk::TreeModel::Row& row)->bool {
				const LISTED_SERVICE& ls = (LISTED_SERVICE) row[services_table_cols.col_service];
				return ls.sid == id.first && ls.scids == id.second;
			}
	);
	if(row_it == children.end())
		return;

	// if the removed service is currently selected, stop playback and deselect it first
	Gtk::TreeModel::iterator current_it = services_table.get_selection()->get_selected();
	if(current_it && current_it == row_it)
		services_table.get_selection()->unselect_all();

	services_table_liststore->erase(row_it);

	RebuildDataSubchannelRows();
	UpdateCUBar();

	{
		std::pair<int,int> key(id.first, id.second == LISTED_SERVICE::scids_none ? 0 : id.second);
		rdns_results.erase(key);
		rdns_tested_params.erase(key);
	}

	if(rdns_window.get_visible())
		rdns_window.SetServices(BuildRDNSServiceList());
	if(links_window.get_visible())
		links_window.SetServices(BuildLinksServiceList());
}

void EDIScopeGTK::FICChangeSubchannelEmitted() {
	std::pair<int,FIC_SUBCHANNEL> change = fic_change_subchannel.Pop();
	known_subchannels[change.first] = change.second;
	RebuildDataSubchannelRows();
	UpdateCUBar();
}

void EDIScopeGTK::FICRemoveSubchannelEmitted() {
	known_subchannels.erase(fic_remove_subchannel.Pop());
	RebuildDataSubchannelRows();
	UpdateCUBar();
}

void EDIScopeGTK::FICChangeDataServiceSIDEmitted() {
	std::pair<int,uint32_t> change = fic_change_data_service_sid.Pop();
	known_data_service_sids[change.first] = change.second;
	RebuildDataSubchannelRows();
	UpdateCUBar();
}

void EDIScopeGTK::FICChangeDataServiceUATypeEmitted() {
	std::pair<uint32_t,int> change = fic_change_data_service_ua_type.Pop();
	known_data_service_ua_types[change.first] = change.second;
	RebuildDataSubchannelRows();
}

void EDIScopeGTK::FICChangeSPIServiceEmitted() {
	SPI_SERVICE_CHANGE change = fic_change_spi_service.Pop();
	fprintf(stderr, "EDIScopeGTK: SubChId %d: SPI service confirmed (packet address %d, FEC %s)\n",
			change.subchid, change.address, change.fec ? "applied" : "not applied");

	StopSPIService();

	spi_subchid = change.subchid;
	spi_mot_decoder = new MOTDirectoryDecoder(this);
	spi_demux = new MSCPacketDemux(change.address, spi_mot_decoder);
	ensemble_player->AddDataSink(spi_subchid, spi_demux);
}

void EDIScopeGTK::StopSPIService() {
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
	spi_window.Clear();
	services_table.queue_draw();	// clears the "S"/"P" columns' checkmarks too
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

void EDIScopeGTK::SPIObjectCompleted(const MOT_DIR_OBJECT& info, const std::vector<uint8_t>& data) {
	// ETSI TS 102 371 table 11: ContentSubType 0 = Service Information, 1 = Programme Information,
	// 2 = Group Information
	switch(info.content_sub_type) {
	case 0: {
		SPI_SERVICE_INFORMATION si;
		if(SPIDecoder::DecodeServiceInformation(data, si))
			spi_info_updated.PushAndEmit(si);
		break;
	}
	case 1: {
		SPI_SCHEDULE schedule;
		if(SPIDecoder::DecodeSchedule(data, schedule)) {
			// some encoders don't embed a <scope><serviceScope> in the schedule itself - fall
			// back to the MOT ScopeID parameter to still know which service this is for
			if(!schedule.has_scope_service)
				schedule.has_scope_service = SPIDecoder::DecodeScopeId(info.scope_id, schedule.ecc, schedule.eid, schedule.sid, schedule.sid_32bit, schedule.scids);
			// last resort: neither was present at all (seen on a real BBC capture) - try the
			// ContentName convention instead, so the schedule is still shown (as "SId 0x...",
			// see EDIScopeGTKSPIWindow::FindServiceName) rather than dropped entirely
			if(!schedule.has_scope_service)
				schedule.has_scope_service = TryExtractSidFromLegacyContentName(info.content_name, schedule.sid, schedule.scids);

			SPIDecoder::DecodeScopeTime(info.scope_start, schedule.scope_start);
			SPIDecoder::DecodeScopeTime(info.scope_end, schedule.scope_end);
			// fall back to the first programme with a time, if the mandatory-in-practice MOT
			// ScopeStart parameter is missing - still enough to bucket this schedule by day
			if(!schedule.scope_start.valid)
				for(const SPI_PROGRAMME& p : schedule.programmes)
					if(p.time.valid) {
						schedule.scope_start = p.time;
						break;
					}

			spi_schedule_updated.PushAndEmit(schedule);
		}
		break;
	}
	case 2: {
		SPI_GROUP_INFORMATION gi;
		if(SPIDecoder::DecodeGroupInformation(data, gi))
			spi_group_info_updated.PushAndEmit(gi);
		break;
	}
	}
}

void EDIScopeGTK::SPIOtherObjectCompleted(const MOT_DIR_OBJECT& info, const std::vector<uint8_t>& data) {
	// expected: a service/ensemble logo (ContentType=image, JFIF or PNG) - anything else is
	// simply not something this window knows how to show, so it's silently ignored
	if(info.content_type != MOT_FILE::CONTENT_TYPE_IMAGE)
		return;

	spi_logo_updated.PushAndEmit(SPI_LOGO_UPDATE{info.content_name, data});
}

void EDIScopeGTK::SPIInfoUpdatedEmitted() {
	spi_info = spi_info_updated.Pop();
	services_table.queue_draw();	// refreshes the "S" column's checkmarks
	if(spi_window.get_visible())
		spi_window.Update(spi_info, spi_logo_data);
}

void EDIScopeGTK::SPILogoUpdatedEmitted() {
	SPI_LOGO_UPDATE update = spi_logo_updated.Pop();
	spi_logo_data[update.content_name] = update.data;
	if(spi_window.get_visible()) {
		spi_window.Update(spi_info, spi_logo_data);
		spi_window.UpdateGroupInformation(spi_group_info, spi_logo_data);	// groups can have logos too
	}
}

void EDIScopeGTK::SPIScheduleUpdatedEmitted() {
	SPI_SCHEDULE schedule = spi_schedule_updated.Pop();
	if(schedule.has_scope_service) {
		std::string date_key = SPIDecoder::FormatDate(schedule.scope_start);
		spi_schedules[std::make_pair(schedule.sid, date_key)] = schedule;
		services_table.queue_draw();	// refreshes the "P" column's checkmarks
	}
	if(spi_window.get_visible())
		spi_window.UpdateSchedule(schedule);
}

void EDIScopeGTK::SPIGroupInfoUpdatedEmitted() {
	spi_group_info = spi_group_info_updated.Pop();
	if(spi_window.get_visible())
		spi_window.UpdateGroupInformation(spi_group_info, spi_logo_data);
}

void EDIScopeGTK::FICRemoveSPIServiceEmitted() {
	int subchid = fic_remove_spi_service.Pop();
	fprintf(stderr, "EDIScopeGTK: SubChId %d: SPI service removed\n", subchid);
	if(subchid == spi_subchid)
		StopSPIService();
}

std::vector<EDIScopeGTKRDNSWindow::SERVICE_ENTRY> EDIScopeGTK::BuildRDNSServiceList() {
	std::vector<EDIScopeGTKRDNSWindow::SERVICE_ENTRY> list;

	for(const Gtk::TreeModel::Row& row : services_table_liststore->children()) {
		const LISTED_SERVICE& service = (LISTED_SERVICE) row[services_table_cols.col_service];
		if(!service.is_audio || service.sid == LISTED_SERVICE::sid_none)
			continue;

		EDIScopeGTKRDNSWindow::SERVICE_ENTRY entry;
		entry.sid = service.sid;
		entry.scids = service.IsPrimary() ? 0 : service.scids;
		entry.ecc = service.ecc != LISTED_SERVICE::ecc_none ? service.ecc : ensemble.ecc;
		entry.eid = ensemble.eid;

		std::string label = FICDecoder::ConvertLabelToUTF8(service.label, nullptr);
		if(service.multi_comps)
			label = (!service.IsPrimary() ? "» " : "") + label + (service.IsPrimary() ? " »" : "");
		entry.label = label;

		list.push_back(entry);
	}

	return list;
}

void EDIScopeGTK::StartRDNSTest(int sid, int scids, int ecc, int eid) {
	std::pair<int,int> key(sid, scids);
	if(rdns_pending.find(key) != rdns_pending.end())
		return;
	rdns_pending.insert(key);
	rdns_tested_params[key] = std::make_pair(ecc, eid);

	// audio services only (see BuildRDNSServiceList()) - always a 16-bit SId
	std::thread([this, sid, scids, ecc, eid]() {
		RDNS_RESULT result = RadioDNSResolver::Resolve(ecc, eid, (uint32_t) sid, false, scids);
		rdns_result_ready.PushAndEmit(RDNS_RESULT_READY{sid, scids, result});
	}).detach();
}

void EDIScopeGTK::MaybeStartRDNSTest(int sid, int scids, int ecc, int eid) {
	if(ecc == FIC_ENSEMBLE::ecc_none || sid == LISTED_SERVICE::sid_none)
		return;

	std::pair<int,int> key(sid, scids);
	if(rdns_pending.find(key) != rdns_pending.end())
		return;

	// already (being) tested with these exact broadcast parameters - a hot reconfiguration that
	// actually changes the SId gives a different key above, and one that changes the ensemble's
	// ECC changes the (ecc,eid) pair here, so either kind naturally falls through to a fresh test
	std::map<std::pair<int,int>,std::pair<int,int>>::const_iterator it = rdns_tested_params.find(key);
	if(it != rdns_tested_params.end() && it->second == std::make_pair(ecc, eid))
		return;

	StartRDNSTest(sid, scids, ecc, eid);
}

void EDIScopeGTK::RescanRDNSServices() {
	for(const EDIScopeGTKRDNSWindow::SERVICE_ENTRY& entry : BuildRDNSServiceList())
		MaybeStartRDNSTest(entry.sid, entry.scids, entry.ecc, entry.eid);
}

void EDIScopeGTK::RDNSResultReadyEmitted() {
	RDNS_RESULT_READY ready = rdns_result_ready.Pop();
	std::pair<int,int> key(ready.sid, ready.scids);
	rdns_results[key] = ready.result;
	rdns_pending.erase(key);

	if(rdns_window.get_visible())
		rdns_window.UpdateResult(ready.sid, ready.scids, ready.result);

	services_table.queue_draw();	// refreshes the "R" column's checkmark/cross
}

std::vector<EDIScopeGTKLinksWindow::SERVICE_ENTRY> EDIScopeGTK::BuildLinksServiceList() {
	std::vector<EDIScopeGTKLinksWindow::SERVICE_ENTRY> list;
	std::set<int> known_sids;

	for(const Gtk::TreeModel::Row& row : services_table_liststore->children()) {
		const LISTED_SERVICE& service = (LISTED_SERVICE) row[services_table_cols.col_service];
		if(!service.is_audio || service.sid == LISTED_SERVICE::sid_none)
			continue;

		EDIScopeGTKLinksWindow::SERVICE_ENTRY entry;
		entry.sid = service.sid;
		entry.scids = service.IsPrimary() ? 0 : service.scids;

		std::string label = FICDecoder::ConvertLabelToUTF8(service.label, nullptr);
		if(service.multi_comps)
			label = (!service.IsPrimary() ? "» " : "") + label + (service.IsPrimary() ? " »" : "");
		entry.label = label;

		list.push_back(entry);
		known_sids.insert(service.sid);
	}

	// a FIG 0/6 linking set isn't necessarily only about services actually carried on this
	// ensemble (clause 8.1.15's "key service" and its DAB_SID members can both be elsewhere,
	// e.g. a service only ever present on another, adjacent ensemble) - add a placeholder entry
	// for every such SId so its linking information can still be inspected, instead of making it
	// impossible to even select that configuration
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

			EDIScopeGTKLinksWindow::SERVICE_ENTRY entry;
			entry.sid = (int) sid;
			entry.scids = 0;
			entry.label = "SId " + StringTools::SidToHex((int) sid) + " (not on this ensemble)";
			list.push_back(entry);
		}
	}

	return list;
}

void EDIScopeGTK::FICChangeServiceLinkingEmitted() {
	SERVICE_LINKING_CHANGE change = fic_change_service_linking.Pop();
	service_linking_sets[change.key] = change.set;

	if(links_window.get_visible())
		links_window.UpdateServiceLinking(change.key, change.set);
}

void EDIScopeGTK::FICChangeFrequencyInfoEmitted() {
	FREQUENCY_INFO_CHANGE change = fic_change_frequency_info.Pop();
	frequency_infos[change.key] = change.info;

	if(links_window.get_visible())
		links_window.UpdateFrequencyInfo(change.key, change.info);
}

void EDIScopeGTK::FICChangeOEServiceEmitted() {
	OE_SERVICE_CHANGE change = fic_change_oe_service.Pop();
	oe_services[change.key] = change.service;

	if(links_window.get_visible())
		links_window.UpdateOEService(change.key, change.service);
}

void EDIScopeGTK::FICRemoveDataServiceSIDEmitted() {
	known_data_service_sids.erase(fic_remove_data_service_sid.Pop());
	RebuildDataSubchannelRows();
	UpdateCUBar();
}

void EDIScopeGTK::FICLogMessageEmitted() {
	logs_window.AppendLogMessage(fic_log_message.Pop());
}

void EDIScopeGTK::FICChangeUTCDateTime(const FIC_DAB_DT& utc_dt) {
	// (re)sync to DAB date/time + update/show/reschedule
	utc_dt_next = utc_dt;
	dt_update = std::chrono::steady_clock::now();
//	fprintf(stderr, "### time synced\n");

	if(dt_lto != FIC_ENSEMBLE::lto_none)
		DoDateTimeSync(FICDecoder::ConvertDateTimeToString(utc_dt, dt_lto, true));

	ShowDateTime(true);
}

void EDIScopeGTK::ShowDateTime(bool scheduled) {
	// if scheduled, update
	if(scheduled)
		utc_dt_curr = utc_dt_next;

	// if UTC date/time and LTO set, show, if output changed
	if(!utc_dt_curr.IsNone() && dt_lto != FIC_ENSEMBLE::lto_none) {
		std::string dt_str = FICDecoder::ConvertDateTimeToString(utc_dt_curr, dt_lto, false);
//		fprintf(stderr, "### time: %s\n", FICDecoder::ConvertDateTimeToString(utc_dt_curr, dt_lto, true).c_str());
		if(dt_str_prev != dt_str) {
			dt_str_prev = dt_str;
//			fprintf(stderr, "### ----- time displayed: %s\n", dt_str.c_str());
			DoDateTimeUpdate(dt_str);
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
			throw std::runtime_error("EDIScopeGTK: error while normalizing date/time");
	}
}

void EDIScopeGTK::EnsembleDoRegularWork() {
	// if UTC date/time set and scheduled time reached, update/show/reschedule
	if(!utc_dt_curr.IsNone() && std::chrono::steady_clock::now() >= dt_update)
		ShowDateTime(true);

	// detect services hot-removed from the ensemble (no longer listed in FIG 0/2)
	fic_decoder->CheckServiceTimeouts();
}

void EDIScopeGTK::DoDateTimeUpdateEmitted() {
	// display updated date/time
	frame_label_datetime.set_sensitive(true);
	label_datetime.set_label(do_datetime_update.Pop());
}

void EDIScopeGTK::DoDateTimeSyncEmitted() {
	// display synced date/time
	frame_label_datetime.set_sensitive(true);
	frame_label_datetime.set_tooltip_text("Last synced date/time: " + do_datetime_sync.Pop());
}

void EDIScopeGTK::FICDiscardedFIB() {
	fprintf(stderr, "\x1B[33m" "(FIB)" "\x1B[0m" " ");
}

void EDIScopeGTK::EnsureEnsemblePlayer(const std::string& format) {
	if(options.source_format == format)
		return;

	// preserve mute/volume across the swap
	bool mute = tglbtn_mute.get_active();
	double volume = vlmbtn.get_value();
	delete ensemble_player;

	options.source_format = format;
	if(format == EnsembleSource::FORMAT_ETI)
		ensemble_player = new ETIPlayer(audio_output_type, options.disable_int_catch_up, this);
	else
		ensemble_player = new EDIPlayer(audio_output_type, options.disable_int_catch_up, this);

	vlmbtn.set_sensitive(ensemble_player->HasAudioVolumeControl());
	ensemble_player->SetAudioMute(mute);
	ensemble_player->SetAudioVolume(volume);
}

void EDIScopeGTK::ShowEnsembleWaitingMessage() {
	// an EDI source's frames are only actually decoded once their own embedded timestamp comes
	// due (see EDIPlayer) - with a mux configured for a large TIST offset/margin, that can hold
	// back the first FIG 0/0 (ensemble label) for that many seconds after the connection itself
	// already succeeded, which otherwise just looks like nothing is happening
	label_ensemble.set_markup("<i>Waiting for timestamp synchronisation...</i>");
	frame_label_ensemble.set_tooltip_text(
			"EDI frames are held back and only decoded once their own embedded timestamp comes due.\n"
			"With a mux configured for a large TIST offset, this can take several seconds - even "
			"though the connection itself already succeeded.");
}

void EDIScopeGTK::StopEnsembleSource() {
	StopSPIService();
	known_service_names.clear();

	if(ensemble_source) {
		ensemble_source->DoExit();
		ensemble_source_thread.join();
		delete ensemble_source;
		ensemble_source = nullptr;
	}

	// frames of the previous connection still waiting for their due time must not refill the FIC
	// (nor play their audio) after the reset below
	ensemble_player->Flush();
	EnsembleResetFIC();
	services_table_liststore->clear();	// TODO: prevent on_services_table_selection_changed() being called for each deleted row
	logs_window.Clear();
	rdns_results.clear();
	rdns_tested_params.clear();
	rdns_pending.clear();
	rdns_window.Clear();
	service_linking_sets.clear();
	frequency_infos.clear();
	oe_services.clear();
	links_window.Clear();
	known_subchannels.clear();
	known_data_service_sids.clear();
	known_data_service_ua_types.clear();
	UpdateCUBar();
	ensemble_player->StopAudio();
	label_ensemble.set_label("");
	frame_label_ensemble.set_tooltip_text("");

	// position progress only applies to a file source - hide it here so it doesn't linger once
	// that source is torn down (e.g. switching over to a live stream); it comes back on its own
	// if the next source is itself a file (see EnsembleUpdateProgressEmitted)
	progress_position.hide();

	frame_label_datetime.set_sensitive(false);
	frame_label_datetime.set_tooltip_text("");
	label_datetime.set_label("");

	frame_label_edi_delay.set_sensitive(false);
	label_edi_delay.set_label("");
	edi_timing_valid = false;

	utc_dt_curr = FIC_DAB_DT();
	dt_lto = FIC_ENSEMBLE::lto_none;
	dt_str_prev = "";
}

void EDIScopeGTK::ShowNetworkConnectError(const std::string& message) {
	Gtk::MessageDialog dialog(*this, message, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
	dialog.set_title("Connection error");
	dialog.run();
}

void EDIScopeGTK::AddToNetworkHistory(const std::string& uri) {
	std::vector<std::string> history = LoadNetworkHistory();
	history.erase(std::remove(history.begin(), history.end(), uri), history.end());
	history.insert(history.begin(), uri);
	if(history.size() > NETWORK_HISTORY_MAX_ENTRIES)
		history.resize(NETWORK_HISTORY_MAX_ENTRIES);
	SaveNetworkHistory(history);

	combo_network_uri.remove_all();
	for(const std::string& h : history)
		combo_network_uri.append(h);
	combo_network_uri.prepend(NETWORK_URI_BROWSE_ITEM);
	combo_network_uri.get_entry()->set_text(uri);
}

void EDIScopeGTK::on_combo_network_uri_changed() {
	if(combo_network_uri.get_active_text() != NETWORK_URI_BROWSE_ITEM)
		return;

	Gtk::FileChooserDialog dialog(*this, "Open EDI/ETI File", Gtk::FILE_CHOOSER_ACTION_OPEN);
	dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
	dialog.add_button("_Open", Gtk::RESPONSE_OK);

	Glib::RefPtr<Gtk::FileFilter> filter_eti_edi = Gtk::FileFilter::create();
	filter_eti_edi->set_name("ETI/EDI files");
	filter_eti_edi->add_pattern("*.eti");
	filter_eti_edi->add_pattern("*.edi");
	dialog.add_filter(filter_eti_edi);

	Glib::RefPtr<Gtk::FileFilter> filter_all = Gtk::FileFilter::create();
	filter_all->set_name("All files");
	filter_all->add_pattern("*");
	dialog.add_filter(filter_all);

	int response = dialog.run();
	if(response == Gtk::RESPONSE_OK) {
		combo_network_uri.get_entry()->set_text("file://" + dialog.get_filename());

		// same as pressing Enter in the entry - start right away, same convenience
		if(!tglbtn_network_connect.get_active())
			tglbtn_network_connect.set_active(true);
	} else {
		combo_network_uri.get_entry()->set_text("");
	}
}

void EDIScopeGTK::on_tglbtn_network_connect() {
	if(tglbtn_network_connect.get_active()) {
		std::string uri = combo_network_uri.get_active_text();

		static const std::string PREFIX_FILE = "file://";
		if(uri.compare(0, PREFIX_FILE.size(), PREFIX_FILE) == 0) {
			std::string path = StringTools::FilePathFromArg(uri);

			// unlike tcp://udp:// (below), a file:// source isn't kept in the dropdown's history -
			// it's one-shot, typically picked once via "Open EDI/ETI File..." and not re-used

			// no way to pick a format in this box - infer it from the extension (defaulting to
			// ETI, same as the CLI's own default) rather than requiring a second field just for this
			std::string lower_path = path;
			std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
			bool is_edi = lower_path.size() >= 4 && lower_path.compare(lower_path.size() - 4, 4, ".edi") == 0;

			StopEnsembleSource();
			EnsureEnsemblePlayer(is_edi ? EnsembleSource::FORMAT_EDI : EnsembleSource::FORMAT_ETI);
			if(is_edi)
				ShowEnsembleWaitingMessage();

			if(is_edi)
				ensemble_source = new EDISource(path, this);
			else
				ensemble_source = new ETISource(path, this);
			ensemble_source_thread = std::thread(&EnsembleSource::Main, ensemble_source);

			combo_network_uri.set_sensitive(false);
			tglbtn_network_connect.set_label("Disconnect");
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
			ShowNetworkConnectError("Unsupported URI - use tcp://host:port or udp://host:port");
			tglbtn_network_connect.set_active(false);
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
			ShowNetworkConnectError("Invalid or missing port in URI '" + uri + "'");
			tglbtn_network_connect.set_active(false);
			return;
		}
		if(is_tcp && host.empty()) {
			ShowNetworkConnectError("tcp:// requires a host");
			tglbtn_network_connect.set_active(false);
			return;
		}

		AddToNetworkHistory(uri);

		// take over from whatever was running before (the initial CLI source)
		StopEnsembleSource();
		EnsureEnsemblePlayer(EnsembleSource::FORMAT_EDI);
		ShowEnsembleWaitingMessage();

		if(is_tcp)
			ensemble_source = new EDITCPSource(host, port, this);
		else
			ensemble_source = new EDIUDPSource(host, port, this);
		ensemble_source_thread = std::thread(&EnsembleSource::Main, ensemble_source);

		combo_network_uri.set_sensitive(false);
		tglbtn_network_connect.set_label("Disconnect");
	} else {
		StopEnsembleSource();

		combo_network_uri.set_sensitive(true);
		tglbtn_network_connect.set_label("Connect");
	}
}

void EDIScopeGTK::on_services_table_selection_changed() {
	LISTED_SERVICE service;	// default: none
	Gtk::TreeModel::iterator row_it = services_table.get_selection()->get_selected();
	if(row_it)
		service = (LISTED_SERVICE) (*row_it)[services_table_cols.col_service];
	SetService(service);

	cu_bar.SetSelectedSubchId(service.audio_service.subchid);
}

void EDIScopeGTK::on_cu_bar_service_clicked(uint16_t sid, int scids) {
	Gtk::ListStore::Children children = services_table_liststore->children();
	Gtk::ListStore::iterator row_it = std::find_if(
			children.begin(), children.end(),
			[&](const Gtk::TreeModel::Row& row)->bool {
				const LISTED_SERVICE& ls = (LISTED_SERVICE) row[services_table_cols.col_service];
				return ls.sid == sid && ls.scids == scids;
			}
	);
	if(row_it != children.end())
		services_table.get_selection()->select(row_it);
}

void EDIScopeGTK::PADChangeDynamicLabelEmitted() {
//	fprintf(stderr, "### PADChangeDynamicLabelEmitted\n");

	DL_STATE dl = pad_change_dynamic_label.Pop();

	// consider clear display command
	if(dl.charset != -1) {
		std::string charset_name;
		std::string label = CharsetTools::ConvertTextToUTF8(&dl.raw[0], dl.raw.size(), dl.charset, false, &charset_name);

		// skip unsupported charsets
		if(!charset_name.empty()) {
			frame_label_dl.set_sensitive(true);
			label_dl.set_label(label);
			frame_label_dl.set_tooltip_text("Charset: " + charset_name);
		}
	} else {
		frame_label_dl.set_sensitive(true);
		label_dl.set_label("");
		frame_label_dl.set_tooltip_text("");
	}

	// if present, forward DL Plus updates
	if(!dl.dl_plus_objects.empty())
		dl_plus_content.UpdateDLPlusInfo(dl);
}

void EDIScopeGTK::PADChangeSlideEmitted() {
//	fprintf(stderr, "### PADChangeSlideEmitted\n");

	slideshow_content.UpdateSlide(pad_change_slide.Pop());
}

void EDIScopeGTK::PADFileProgressEmitted() {
//	fprintf(stderr, "### PADFileProgressEmitted\n");

	slideshow_content.UpdateFileProgress(pad_file_progress.Pop());
}

void EDIScopeGTK::PADLengthError(size_t /*announced_xpad_len*/, size_t /*xpad_len*/) {
	fprintf(stderr, "\x1B[31m" "[X-PAD len]" "\x1B[0m" " ");
}
