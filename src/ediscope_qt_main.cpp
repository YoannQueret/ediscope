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
#include "ediscope_icon.h"

#include <signal.h>
#include <unistd.h>

#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QSettings>
#include <QSocketNotifier>


// SIGINT/SIGTERM are turned into a byte on this pipe, read back on the UI thread (a signal
// handler itself must not touch Qt)
static int signal_pipe[2] = {-1, -1};

static void break_handler(int) {
	char c = 1;
	if(write(signal_pipe[1], &c, 1) == -1) {
		// nothing more that can safely be done in a signal handler
	}
}


static void usage(const char* exe) {
	EDIScopeQtOptions options_default;

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
					"  file         Input file to be played (stdin, if something is piped in) - a file:// URI works too\n",
					EnsembleSource::FORMAT_ETI.c_str(),
					EnsembleSource::FORMAT_EDI.c_str(),
					options_default.recordings_path.c_str(),
					options_default.rec_prebuffer_size_s
			);
	exit(1);
}


int main(int argc, char **argv) {
	EDIScopeQtOptions options;
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
			options.recordings_path_given = true;
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
	if(options.source_format != EnsembleSource::FORMAT_ETI && options.source_format != EnsembleSource::FORMAT_EDI) {
		fprintf(stderr, "Unknown source format '%s'!\n", options.source_format.c_str());
		usage(argv[0]);
	}
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
	if(initial_param_count > 1) {
		fprintf(stderr, "At most one initial parameter shall be specified!\n");
		usage(argv[0]);
	}

	fprint_ediscope_banner(stderr);

	// Qt gets no command line arguments of its own: every option above is EDIScope's
	static int qt_argc = 1;
	QApplication app(qt_argc, argv);
	QApplication::setApplicationName("ediscope");
	QApplication::setApplicationDisplayName("EDIScope");
	QApplication::setApplicationVersion(EDISCOPE_VERSION);
	QGuiApplication::setDesktopFileName("ediscope");	// Wayland app id, matches ediscope.desktop

	QApplication::setOrganizationName("ediscope");	// settings in ~/.config/ediscope/ediscope.conf
	EDIScopeQtTheme::Apply(app);

	// the recordings folder chosen in Settings, unless given on the command line
	if(!options.recordings_path_given) {
		QString saved = QSettings().value("recordings/path").toString();
		if(!saved.isEmpty())
			options.recordings_path = saved.toStdString();
	}

	// fixed app icon (taskbar/window list) for every window this app creates
	QPixmap icon_pixmap;
	icon_pixmap.loadFromData(ediscope_icon_png, ediscope_icon_png_len, "PNG");
	QApplication::setWindowIcon(QIcon(icon_pixmap));

	EDIScopeQt *ediscope = new EDIScopeQt(options);

	// handle signals: close the window (and so quit) on the UI thread
	if(pipe(signal_pipe) == -1) {
		perror("EDIScope: error while creating signal pipe");
		return 1;
	}
	QSocketNotifier signal_notifier(signal_pipe[0], QSocketNotifier::Read);
	QObject::connect(&signal_notifier, &QSocketNotifier::activated, [&]() {
		char c;
		if(read(signal_pipe[0], &c, 1) == -1) {
			// nothing to do - the notifier fires again if a byte is still there
		}
		fprintf(stderr, "...EDIScope exits...\n");
		// exit() rather than quit(): the latter first asks every window to close, which the
		// Links/RadioDNS/SPI windows refuse (they only hide) - a recording in progress is
		// completed by the main window's destructor
		QCoreApplication::exit(0);
	});
	if(signal(SIGINT, break_handler) == SIG_ERR) {
		perror("EDIScope: error while setting SIGINT handler");
		return 1;
	}
	if(signal(SIGTERM, break_handler) == SIG_ERR) {
		perror("EDIScope: error while setting SIGTERM handler");
		return 1;
	}

	ediscope->show();
	int result = app.exec();
	delete ediscope;

	return result;
}
