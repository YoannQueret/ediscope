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

#include "eti_source.h"

#include <chrono>


// --- EnsembleSource -----------------------------------------------------------------
const std::string EnsembleSource::FORMAT_ETI = "eti";
const std::string EnsembleSource::FORMAT_EDI = "edi";

EnsembleSource::EnsembleSource(std::string filename, EnsembleSourceObserver *observer, std::string format_name, size_t initial_frame_size) {
	this->filename = filename;
	this->observer = observer;
	this->format_name = format_name;
	this->initial_frame_size = initial_frame_size;

	sync_magics_max_len = 0;

	input_file = nullptr;
#ifdef _WIN32
	input_socket = INVALID_SOCKET;
#endif
	ensemble_frame.resize(initial_frame_size);
	ensemble_frames_count = 0;
	ensemble_bytes_count = 0;
	ensemble_bytes_total = 0;
	ensemble_progress_next_ms = 0;

	do_exit = false;
}

EnsembleSource::~EnsembleSource() {
	// cleanup
#ifdef _WIN32
	CloseInput();
#else
	if(input_file && input_file != stdin)
		fclose(input_file);
#endif
}

#ifdef _WIN32
void EnsembleSource::CloseInput() {
	if(input_file && input_file != stdin)
		fclose(input_file);
	input_file = nullptr;
	if(input_socket != INVALID_SOCKET)
		closesocket(input_socket);
	input_socket = INVALID_SOCKET;
}

int EnsembleSource::ReadInput(uint8_t *buf, size_t len) {
	if(input_socket != INVALID_SOCKET) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(input_socket, &fds);
		timeval select_timeval;
		select_timeval.tv_sec = 0;
		select_timeval.tv_usec = 100 * 1000;

		int ready_fds = select(0, &fds, nullptr, nullptr, &select_timeval);
		if(ready_fds == SOCKET_ERROR) {
			fprintf(stderr, "EnsembleSource: error while select (%d)\n", WSAGetLastError());
			return READ_ERROR;
		}
		if(!ready_fds)
			return READ_NOTHING;

		int bytes = recv(input_socket, (char*) buf, (int) len, 0);
		if(bytes == SOCKET_ERROR) {
			fprintf(stderr, "EnsembleSource: error while recv (%d)\n", WSAGetLastError());
			return READ_ERROR;
		}
		return bytes;	// 0: connection closed, as EOF
	}

	// a file (or stdin): select() only works on sockets on Windows - a file is always readable anyway
	size_t bytes = fread(buf, 1, len, input_file);
	if(bytes)
		return (int) bytes;
	if(feof(input_file))
		return READ_EOF;
	perror("EnsembleSource: error while fread");
	return READ_ERROR;
}
#endif

void EnsembleSource::PrintSource() {
	fprintf(stderr, "EnsembleSource: reading %s from '%s'\n", format_name.c_str(), filename.c_str());
}

bool EnsembleSource::OpenFile() {
	if(filename.empty()) {
		input_file = stdin;
		filename = "stdin";
	} else {
#ifdef _WIN32
		input_file = FileTools::OpenUTF8(filename, "rb");
#else
		input_file = fopen(filename.c_str(), "rb");
#endif
		if(!input_file) {
			perror("EnsembleSource: error opening input file");
			return false;
		}
	}

	// init total bytes
	return UpdateTotalBytes();
}

bool EnsembleSource::UpdateTotalBytes() {
	// if file size available, get total bytes count
	off_t old_offset = ftello(input_file);
	if(old_offset == -1) {
		// ignore non-seekable files (like usually stdin)
		if(errno == ESPIPE)
			return true;
		perror("EnsembleSource: error getting file offset");
		return false;
	}

	if(fseeko(input_file, 0, SEEK_END)) {
		perror("EnsembleSource: error seeking to end");
		return false;
	}

	off_t len = ftello(input_file);
	if(len == -1) {
		perror("EnsembleSource: error getting file size");
		return false;
	}

	if(fseeko(input_file, old_offset, SEEK_SET)) {
		perror("EnsembleSource: error seeking to offset");
		return false;
	}

	ensemble_bytes_total = len;
	return true;
}

static const int RECONNECT_DELAY_S = 5;

bool EnsembleSource::WaitBeforeReconnect() {
	// slept in small slices so a do_exit request (the user hitting "disconnect", or the window
	// closing) is picked up quickly instead of after a full RECONNECT_DELAY_S
	for(int waited_ms = 0; waited_ms < RECONNECT_DELAY_S * 1000; waited_ms += 100) {
		if(do_exit)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return !do_exit;
}

int EnsembleSource::Main() {
	while(true) {
		Init();

#ifdef _WIN32
		if(!HasInput()) {
#else
		if(!input_file) {
#endif
			if(!IsReconnectable()) {
				// plain file/stdin source: Init() does nothing, this is the regular first-time open
				if(!OpenFile())
					return 1;
			} else {
				// network source: Init() already reported why the (re)connect attempt failed
				fprintf(stderr, "EnsembleSource: connect failed, retrying in %ds...\n", RECONNECT_DELAY_S);
				if(!WaitBeforeReconnect())
					return 0;
				continue;
			}
		}

		PrintSource();
		int result = ReadLoop();

#ifdef _WIN32
		CloseInput();
#else
		if(input_file && input_file != stdin) {
			fclose(input_file);
			input_file = nullptr;
		}
#endif

		if(!IsReconnectable() || do_exit)
			return result;

		fprintf(stderr, "EnsembleSource: connection lost, reconnecting in %ds...\n", RECONNECT_DELAY_S);
		if(!WaitBeforeReconnect())
			return 0;
	}
}

int EnsembleSource::ReadLoop() {
#ifndef _WIN32
	int file_no = fileno(input_file);

	// set non-blocking mode
	int old_flags = fcntl(file_no, F_GETFL);
	if(old_flags == -1) {
		perror("EnsembleSource: error getting socket flags");
		return 1;
	}
	if(fcntl(file_no, F_SETFL, (old_flags == -1 ? 0 : old_flags) | O_NONBLOCK)) {
		perror("EnsembleSource: error setting socket flags");
		return 1;
	}
#endif

	// a connection lost in the middle of a frame leaves the buffer sized for that frame: start the
	// new connection from the initial (header-only) size, like the very first one
	ensemble_frame.resize(initial_frame_size);

#ifndef _WIN32
	fd_set fds;
	timeval select_timeval;
#endif
	size_t filled = 0;
	size_t sync_skipped = 0;

	while(!do_exit) {
		// use loop to do some regular work
		observer->EnsembleDoRegularWork();

#ifdef _WIN32
		int read_result = ReadInput(&ensemble_frame[filled], ensemble_frame.size() - filled);
		if(read_result == READ_NOTHING)
			continue;
		if(read_result == READ_ERROR)
			return 1;
		size_t bytes = read_result;
#else
		FD_ZERO(&fds);
		FD_SET(file_no, &fds);

		select_timeval.tv_sec = 0;
		select_timeval.tv_usec = 100 * 1000;

		int ready_fds = select(file_no + 1, &fds, nullptr, nullptr, &select_timeval);
		if(ready_fds == -1) {
			// ignore break request, as handled above
			if(errno != EINTR)
				perror("EnsembleSource: error while select");
			continue;
		}
		if(!(ready_fds && FD_ISSET(file_no, &fds)))
			continue;

		size_t bytes = fread(&ensemble_frame[filled], 1, ensemble_frame.size() - filled, input_file);
#endif

		if(bytes > 0) {
			ensemble_bytes_count += bytes;
			filled += bytes;
		}
		if(bytes == 0) {
#ifdef _WIN32
			{	// ReadInput() only returns 0 at EOF
#else
			if(feof(input_file)) {
#endif
				size_t sync_skipped_eof = sync_skipped + filled;
				if(sync_skipped_eof)
					fprintf(stderr, "EnsembleSource: skipping %zu bytes at EOF\n", sync_skipped_eof);
				fprintf(stderr, "EnsembleSource: EOF reached!\n");

				// if present, update progress
				if(ensemble_bytes_total) {
					if(!UpdateProgress())
						return 1;
				}
				break;
			}
			perror("EnsembleSource: error while fread");
			return 1;
		}

		if(filled < ensemble_frame.size())
			continue;

		// check for frame sync i.e. if any sync magic matches
		size_t offset;
		sync_magics_t::const_iterator matched_sync_magic = sync_magics.cend();
		for(offset = 0; offset < ensemble_frame.size() - (sync_magics_max_len - 1); offset++) {
			matched_sync_magic = std::find_if(sync_magics.cbegin(), sync_magics.cend(), [&](const SYNC_MAGIC& sm)->bool {return sm.matches(&ensemble_frame[offset]);});
			if(matched_sync_magic != sync_magics.cend())
				break;
		}

		if(offset) {	// buffer not (yet) synced
			// discard buffer start
			memmove(&ensemble_frame[0], &ensemble_frame[offset], ensemble_frame.size() - offset);
			filled -= offset;
			sync_skipped += offset;
		} else {		// buffer synced
			if(sync_skipped) {
				fprintf(stderr, "EnsembleSource: skipping %zu bytes for sync\n", sync_skipped);
				sync_skipped = 0;
			}

			if(CheckFrameCompleted(*matched_sync_magic)) {
				ensemble_frames_count++;

				// if present, update progress every 500ms
				if(ensemble_bytes_total && ensemble_frames_count * 24 >= ensemble_progress_next_ms) {
					if(!UpdateProgress())
						return 1;
					ensemble_progress_next_ms += 500;
				}

				ProcessCompletedFrame(*matched_sync_magic);

				ensemble_frame.resize(initial_frame_size);
				filled = 0;
			}
		}
	}

	return 0;
}

void EnsembleSource::AddSyncMagic(size_t offset, std::vector<uint8_t> bytes, std::string name) {
	SYNC_MAGIC sm(offset, bytes, name);
	sync_magics.push_back(sm);
	if(sm.len() > sync_magics_max_len)
		sync_magics_max_len = sm.len();
}

bool EnsembleSource::UpdateProgress() {
	// update total bytes
	if(!UpdateTotalBytes())
		return false;

	size_t ensemble_bytes_left = ensemble_bytes_total - ensemble_bytes_count;
	double ensemble_frame_size_avg = ensemble_bytes_count / (double) ensemble_frames_count;
	size_t ensemble_frames_left = ensemble_bytes_left / ensemble_frame_size_avg;
	size_t ensemble_frames_total = ensemble_frames_count + ensemble_frames_left;

	ENSEMBLE_PROGRESS progress;
	progress.value = (double) ensemble_frames_count / (double) ensemble_frames_total;
	progress.text = StringTools::MsToTimecode(ensemble_frames_count * 24) + " / " + StringTools::MsToTimecode(ensemble_frames_total * 24);
	observer->EnsembleUpdateProgress(progress);
	return true;
}
