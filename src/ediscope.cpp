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

#include "ediscope.h"

#include "edi_network_source.h"

static EDIScopeText *ediscope = nullptr;

static void break_handler(int) {
	fprintf(stderr, "...EDIScope exits...\n");
	if(ediscope)
		ediscope->DoExit();
}


// a filename of "tcp://host:port" or "udp://host:port" is a live EDI/TCP or EDI/UDP source
// instead of a local file - mirrors EDIScopeGTK::on_tglbtn_network_connect()'s own URI parsing
struct NETWORK_URI {
	bool matched;	// "tcp://" or "udp://" prefix recognized, whether or not the rest parses fine
	bool valid;		// matched, and host:port parsed fine too
	bool is_tcp;
	std::string host;
	int port;
};

static NETWORK_URI ParseNetworkURI(const std::string& uri) {
	static const std::string PREFIX_TCP = "tcp://";
	static const std::string PREFIX_UDP = "udp://";

	NETWORK_URI result{false, false, false, "", -1};

	if(uri.compare(0, PREFIX_TCP.size(), PREFIX_TCP) == 0)
		result.is_tcp = true;
	else if(uri.compare(0, PREFIX_UDP.size(), PREFIX_UDP) == 0)
		result.is_tcp = false;
	else
		return result;
	result.matched = true;

	std::string rest = uri.substr(result.is_tcp ? PREFIX_TCP.size() : PREFIX_UDP.size());
	size_t colon_pos = rest.find_last_of(':');
	result.host = colon_pos == std::string::npos ? "" : rest.substr(0, colon_pos);
	std::string port_str = colon_pos == std::string::npos ? "" : rest.substr(colon_pos + 1);

	try {
		size_t idx;
		result.port = std::stoi(port_str, &idx);
		if(idx != port_str.size())
			result.port = -1;
	} catch(const std::exception&) {
		result.port = -1;
	}

	result.valid = result.port > 0 && result.port <= 65535 && (!result.is_tcp || !result.host.empty());
	return result;
}


static void usage(const char* exe) {
	fprint_ediscope_banner(stderr);
	fprintf(stderr, "Usage: %s [OPTIONS] [file]\n", exe);
	fprintf(stderr, "  -h            Show this help\n"
					"  -f <format>   Source format: \"%s\" (default), \"%s\"\n"
					"  -l <label>    Label of the service to be played\n"
					"  -1            Play the first service found\n"
					"  -s <sid>      ID of the service to be played\n"
					"  -x <scids>    ID of the service component to be played (requires service ID)\n"
					"  -r <subchid>  ID of the sub-channel (DAB) to be played\n"
					"  -R <subchid>  ID of the sub-channel (DAB+) to be played\n"
					"  -p            Output raw PCM to stdout instead of using SDL\n"
					"  -w            Output RIFF WAVE with PCM to stdout instead of using SDL (useful only on Little Endian)\n"
					"  -u            Output untouched audio stream to stdout instead of using SDL\n"
					"  -I            Don't catch up on stream after interruption\n"
					"  -F            Disable dynamic FIC messages (dynamic PTY, announcements)\n"
					"  file          Input file to be played (stdin, if not specified) - a file:// URI\n"
					"                works too, as does tcp://host:port or udp://host:port for a live EDI source\n",
					EnsembleSource::FORMAT_ETI.c_str(),
					EnsembleSource::FORMAT_EDI.c_str()
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

	EDIScopeTextOptions options;
	int initial_param_count = 0;

	// option args
	int c;
	while((c = getopt(argc, argv, "hf:l:s:x:1pwuIFr:R:")) != -1) {
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
			options.initial_subchid_dab = strtol(optarg, nullptr, 0);
			initial_param_count++;
			break;
		case 'R':
			options.initial_subchid_dab_plus = strtol(optarg, nullptr, 0);
			initial_param_count++;
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

	ediscope = new EDIScopeText(options);
	int result = ediscope->Main();
	delete ediscope;

	return result;
}



// --- EDIScopeText -----------------------------------------------------------------
EDIScopeText::EDIScopeText(EDIScopeTextOptions options) {
	this->options = options;

	// set XTerm window title to version string
	fprintf(stderr, "\x1B]0;" "EDIScope v" EDISCOPE_VERSION "\a");

	AudioOutputType audio_output_type = AudioOutputType::SDL;
#ifndef EDISCOPE_DISABLE_SDL
	if(options.pcm_output)
#endif
		audio_output_type = AudioOutputType::PCM;
	if(options.wav_output)
		audio_output_type = AudioOutputType::WAV;
	if(options.untouched_output)
		audio_output_type = AudioOutputType::Untouched;

	NETWORK_URI network_uri = ParseNetworkURI(options.filename);
	if(network_uri.matched && !network_uri.valid) {
		fprintf(stderr, "EDIScope: invalid network URI '%s' - use tcp://host:port or udp://host:port\n", options.filename.c_str());
		exit(1);
	}

	// a network source is EDI-only (no live ETI/TCP or ETI/UDP equivalent), same as the GUI
	if(options.source_format == EnsembleSource::FORMAT_ETI && !network_uri.valid)
		ensemble_player = new ETIPlayer(audio_output_type, options.disable_int_catch_up, this);
	else
		ensemble_player = new EDIPlayer(audio_output_type, options.disable_int_catch_up, this);

	// set initial sub-channel, if desired
	if(options.initial_subchid_dab != AUDIO_SERVICE::subchid_none) {
		ensemble_player->SetAudioService(AUDIO_SERVICE(options.initial_subchid_dab, false));

		// set XTerm window title to sub-channel number
		fprintf(stderr, "\x1B]0;" "Sub-channel %d (DAB) - EDIScope" "\a", options.initial_subchid_dab);
	}
	if(options.initial_subchid_dab_plus != AUDIO_SERVICE::subchid_none) {
		ensemble_player->SetAudioService(AUDIO_SERVICE(options.initial_subchid_dab_plus, true));

		// set XTerm window title to sub-channel number
		fprintf(stderr, "\x1B]0;" "Sub-channel %d (DAB+) - EDIScope" "\a", options.initial_subchid_dab_plus);
	}

	if(network_uri.valid)
		ensemble_source = network_uri.is_tcp ?
				(EnsembleSource*) new EDITCPSource(network_uri.host, network_uri.port, this) :
				(EnsembleSource*) new EDIUDPSource(network_uri.host, network_uri.port, this);
	else if(options.source_format == EnsembleSource::FORMAT_ETI)
		ensemble_source = new ETISource(options.filename, this);
	else
		ensemble_source = new EDISource(options.filename, this);

	fic_decoder = new FICDecoder(this, options.disable_dyn_fic_msgs);
}

EDIScopeText::~EDIScopeText() {
	DoExit();
	delete ensemble_source;
	delete ensemble_player;
	delete fic_decoder;
}

void EDIScopeText::EnsembleUpdateProgress(const ENSEMBLE_PROGRESS& progress) {
	// compensate cursor movement
	std::string format = "\x1B[34m" "%s" "\x1B[0m";
	format.append(progress.text.length(), '\b');

	fprintf(stderr, format.c_str(), progress.text.c_str());
}

void EDIScopeText::EnsembleEDITiming(const EDI_TIMING& timing) {
	if(timing.valid)
		fprintf(stderr, "EDIPlayer: buffer delay %.0f ms, margin %.0f ms, TIST@FCT0 %.0f ms\n",
				timing.buffer_delay_ms, timing.margin_delay_ms, timing.tist_at_fct0_ms);
}

void EDIScopeText::FICChangeService(const LISTED_SERVICE& service) {
//	fprintf(stderr, "### FICChangeService\n");

	std::string label = FICDecoder::ConvertLabelToUTF8(service.label, nullptr);

	// if first found service requested, adopt service params (for possible later changes)
	if(options.initial_first_found_service) {
		options.initial_sid = service.sid;
		options.initial_scids = service.scids;
		options.initial_first_found_service = false;
	}

	// abort, if no/not initial service
	if(!(label == options.initial_label || (service.sid == options.initial_sid && service.scids == options.initial_scids)))
		return;

	// if the audio service changed, switch
	if(!ensemble_player->IsSameAudioService(service.audio_service))
		ensemble_player->SetAudioService(service.audio_service);

	// set XTerm window title to service name
	fprintf(stderr, "\x1B]0;" "%s - EDIScope" "\a", label.c_str());
}

void EDIScopeText::FICDiscardedFIB() {
	fprintf(stderr, "\x1B[33m" "(FIB)" "\x1B[0m" " ");
}
