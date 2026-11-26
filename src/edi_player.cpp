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

#include "edi_player.h"

#include <cmath>


// --- EDIPlayer -----------------------------------------------------------------
EDIPlayer::EDIPlayer(AudioOutputType audio_output_type, bool disable_int_catch_up, EnsemblePlayerObserver *observer)
		: EnsemblePlayer(audio_output_type, disable_int_catch_up, observer) {
	do_exit = false;
	flush_generation = 0;
	last_margin_delay_ms = 0.0;
	last_buffer_delay_ms = 0.0;
	last_tist_at_fct0_ms = 0.0;

	playback_thread = std::thread(&EDIPlayer::PlaybackThreadMain, this);
}

EDIPlayer::~EDIPlayer() {
	PrepareStop();
	playback_thread.join();
}

void EDIPlayer::PrepareStop() {
	EnsemblePlayer::PrepareStop();

	std::lock_guard<std::mutex> lock(queue_mutex);
	do_exit = true;
	queue_cv.notify_one();
}

void EDIPlayer::Flush() {
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		queue.clear();
		flush_generation++;
	}
	// wakes up a playback thread waiting for a (now dropped) frame's due time
	queue_cv.notify_one();

	// a frame already being decoded finishes first; any later one sees the new generation
	std::lock_guard<std::mutex> lock(playback_mutex);
}

void EDIPlayer::DecodeFrame(const uint8_t *edi_frame) {
	// SYNC
	uint16_t sync = edi_frame[0] << 8 | edi_frame[1];
	switch(sync) {
	case 0x4146:	// AF
		// supported
		break;
	case 0x5046:	// PF
		fprintf(stderr, "EDIPlayer: ignored unsupported EDI PF packet\n");
		return;
	default:
		fprintf(stderr, "EDIPlayer: ignored EDI packet with SYNC = 0x%04X\n", sync);
		return;
	}

	// LEN
	size_t len = edi_frame[2] << 24 | edi_frame[3] << 16 | edi_frame[4] << 8 | edi_frame[5];

	// CF
	bool cf = edi_frame[8] & 0x80;
	if(!cf) {
		fprintf(stderr, "EDIPlayer: ignored EDI AF packet without CRC\n");
		return;
	}

	// MAJ
	int maj = (edi_frame[8] & 0x70) >> 4;
	if(maj != 0x01) {
		fprintf(stderr, "EDIPlayer: ignored EDI AF packet with MAJ = 0x%02X\n", maj);
		return;
	}

	// MIN
	int min = edi_frame[8] & 0x0F;
	if(min != 0x00) {
		fprintf(stderr, "EDIPlayer: ignored EDI AF packet with MIN = 0x%02X\n", min);
		return;
	}

	// PT
	if(edi_frame[9] != 'T') {
		fprintf(stderr, "EDIPlayer: ignored EDI AF packet with PT = '%c'\n", edi_frame[9]);
		return;
	}

	// check CRC
	uint16_t crc_stored = edi_frame[10 + len] << 8 | edi_frame[10 + len + 1];
	uint16_t crc_calced = CalcCRC::CalcCRC_CRC16_CCITT.Calc(edi_frame, 10 + len);
	if(crc_stored != crc_calced) {
		fprintf(stderr, "EDIPlayer: ignored EDI AF packet due to wrong CRC\n");
		return;
	}

	size_t frame_len = 10 + len + 2;

	// peek the "deti" TAG item's ATST (if present and usable) to learn this frame's own schedule,
	// purely to decide whether to admit it into the playback queue (and to report the margin
	// below) - the actual decode (FIC/audio, with full re-validation) happens later, in
	// PlaybackFrame, so any warning for a malformed TAG item is left to that pass, unchanged
	std::chrono::system_clock::time_point frame_time;
	bool has_time = false;
	int fct = -1;

	size_t tag_item_len_bytes;
	for(size_t i = 0; i < len - 8; i += tag_item_len_bytes) {
		const uint8_t* tag_item = edi_frame + 10 + i;

		std::string tag_name((const char*) tag_item, 4);
		size_t tag_len = tag_item[4] << 24 | tag_item[5] << 16 | tag_item[6] << 8 | tag_item[7];
		const uint8_t* tag_value = tag_item + 8;

		tag_item_len_bytes = 4 + 4 + (tag_len + 7) / 8;

		if(tag_name != "deti")
			continue;

		bool atstf = tag_value[0] & 0x80;
		bool ficf = tag_value[0] & 0x40;
		bool rfudf = tag_value[0] & 0x20;

		if(!(tag_value[2] == 0xFF || tag_value[2] == 0xF0))
			break;	// invalid STAT - leave entirely to the full pass below

		fct = tag_value[1];

		int mid = tag_value[3] >> 6;
		size_t fic_len = ficf ? (mid == 3 ? 128 : 96) : 0;
		size_t tag_len_bytes_calced = 2 + 4 + (atstf ? 8 : 0) + fic_len + (rfudf ? 3 : 0);
		if(tag_len != tag_len_bytes_calced * 8)
			break;	// malformed - same, leave to the full pass

		if(!atstf)
			break;	// no timestamp carried - play back immediately, as always

		// ATST: UTCO (1 byte) + seconds since 2000-01-01T00:00:00Z, UTCO-adjusted (4 bytes,
		// big-endian) + TSTA, a 24bit fine timestamp in units of 1/16384000s (big-endian) -
		// ETSI TS 102 693, 5.1.3; decoded exactly as ODR-DabMux's own EdiDecoder does
		// (lib/edi/common.cpp, frame_timestamp_t::to_unix_epoch()/to_system_clock())
		const uint8_t *atst = tag_value + 2 + 4;
		uint8_t utco = atst[0];
		uint32_t seconds = (uint32_t) atst[1] << 24 | atst[2] << 16 | atst[3] << 8 | atst[4];
		uint32_t tsta = (uint32_t) atst[5] << 16 | atst[6] << 8 | atst[7];

		static const time_t edi_epoch_2000 = 946684800;
		time_t unix_seconds = edi_epoch_2000 + (time_t) seconds - utco;

		frame_time = std::chrono::system_clock::from_time_t(unix_seconds) +
				std::chrono::microseconds(std::lround(tsta / 16.384));
		has_time = true;
		break;
	}

	std::chrono::system_clock::time_point sys_now = std::chrono::system_clock::now();

	if(has_time) {
		// margin: now (at actual network reception, before any buffering) minus this frame's own
		// schedule - negative under healthy conditions, reported for every single timestamped
		// frame, even ones about to be dropped below
		double margin_delay_ms = std::chrono::duration<double, std::milli>(sys_now - frame_time).count();

		EDI_TIMING timing;
		timing.valid = true;
		timing.fct = fct;
		{
			std::lock_guard<std::mutex> lock(timing_mutex);
			last_margin_delay_ms = margin_delay_ms;
			timing.margin_delay_ms = margin_delay_ms;
			timing.buffer_delay_ms = last_buffer_delay_ms;
			timing.tist_at_fct0_ms = last_tist_at_fct0_ms;
		}
		ReportEDITiming(timing);

		// already due (or overdue) by the time it actually arrived - drop it outright, rather
		// than playing a backlog back faster than real time (e.g. a mux's TCP preroll burst on a
		// fresh connection): the playback queue only ever holds frames still in the future
		if(frame_time <= sys_now)
			return;
	}

	QueuedFrame frame;
	frame.data.assign(edi_frame, edi_frame + frame_len);
	frame.frame_time = frame_time;
	frame.has_time = has_time;

	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		queue.push_back(std::move(frame));
	}
	queue_cv.notify_one();
}

void EDIPlayer::PlaybackThreadMain() {
	while(true) {
		QueuedFrame frame;
		unsigned long generation;
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait(lock, [this]{return do_exit || !queue.empty();});
			// on shutdown, stop right away rather than draining whatever is still queued (up to a
			// few seconds' worth of already-admitted, not-yet-due frames) at real-time pace first
			if(do_exit)
				return;
			frame = std::move(queue.front());
			queue.pop_front();
			generation = flush_generation;
		}

		if(frame.has_time) {
			// convert the (system_clock) target into the steady_clock domain the wait below needs,
			// by carrying over the offset between two "now"s sampled an instant apart - a wall-
			// clock step (e.g. NTP) in between would skew this by that same step, self-correcting
			// again on the very next frame
			std::chrono::system_clock::time_point sys_now = std::chrono::system_clock::now();
			std::chrono::steady_clock::time_point steady_now = std::chrono::steady_clock::now();
			std::chrono::steady_clock::time_point target = steady_now +
					std::chrono::duration_cast<std::chrono::steady_clock::duration>(frame.frame_time - sys_now);

			// wait_until() rather than a bare sleep_until(), so a shutdown request (do_exit,
			// signaled through the same condition variable by PrepareStop()) wakes this up
			// immediately instead of only after the full wait - which, right after connecting to
			// a mux with a multi-second tist_offset, can be as long as that offset itself for the
			// very first frame
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait_until(lock, target, [this, generation]{return do_exit || flush_generation != generation;});
			if(do_exit)
				return;
		}

		// checked again with playback_mutex held, so Flush() either drops this frame or waits for it
		std::lock_guard<std::mutex> playback_lock(playback_mutex);
		{
			std::lock_guard<std::mutex> lock(queue_mutex);
			if(do_exit)
				return;
			if(flush_generation != generation)
				continue;
		}
		PlaybackFrame(&frame.data[0]);
	}
}

void EDIPlayer::PlaybackFrame(const uint8_t *edi_frame) {
	size_t len = edi_frame[2] << 24 | edi_frame[3] << 16 | edi_frame[4] << 8 | edi_frame[5];

	// parse TAG packet for TAG items (skipping any TAG packet padding)
	size_t tag_item_len_bytes;
	for(size_t i = 0; i < len - 8; i += tag_item_len_bytes) {
		const uint8_t* tag_item = edi_frame + 10 + i;

		std::string tag_name((const char*) tag_item, 4);
		size_t tag_len = tag_item[4] << 24 | tag_item[5] << 16 | tag_item[6] << 8 | tag_item[7];
		const uint8_t* tag_value = tag_item + 8;

		tag_item_len_bytes = 4 + 4 + (tag_len + 7) / 8;

		// protocol type and revision
		if(tag_name == "*ptr") {
			if(tag_len != 64) {
				fprintf(stderr, "EDIPlayer: ignored *ptr TAG item with wrong length (%zu bits)\n", tag_len);
				continue;
			}

			std::string protocol_type((const char*) tag_value, 4);
			if(protocol_type != "DETI")
				fprintf(stderr, "EDIPlayer: unsupported protocol type '%s' in *ptr TAG item\n", protocol_type.c_str());

			int major = tag_value[4] << 8 | tag_value[5];
			int minor = tag_value[6] << 8 | tag_value[7];
			if(major != 0x0000 || minor != 0x0000)
				fprintf(stderr, "EDIPlayer: unsupported major/minor revision 0x%04X/0x%04X in *ptr TAG item\n", major, minor);

			continue;
		}

		// dummy padding - ignored
		if(tag_name == "*dmy")
			continue;

		// DAB ETI(LI) Management
		if(tag_name == "deti") {
			// flag field
			bool atstf = tag_value[0] & 0x80;
			bool ficf = tag_value[0] & 0x40;
			bool rfudf = tag_value[0] & 0x20;

			// STAT
			// 0xFF Error level 0: LIDATA contains no detectable errors (which shall be the default state, set at the origin of LIDATA).
			// 0xF0 Error level 1: LIDATA contains errors which can be ignored by the processing equipment.
			// 0x0F Error level 2: LIDATA contains errors which should not be ignored by the processing
			//                     equipment but which may be mitigated by additional processing within that
			//                     equipment, for example by using header information from previous frames.
			// 0x00 Error level 3: LIDATA is not valid and the processing equipment should mute.
			if(!(tag_value[2] == 0xFF || tag_value[2] == 0xF0)) {
				fprintf(stderr, "EDIPlayer: EDI AF packet with STAT = 0x%02X\n", tag_value[2]);
				continue;
			}

			int mid = tag_value[3] >> 6;
			size_t fic_len = ficf ? (mid == 3 ? 128 : 96) : 0;

			size_t tag_len_bytes_calced = 2 + 4 + (atstf ? 8 : 0) + fic_len + (rfudf ? 3 : 0);
			if(tag_len != tag_len_bytes_calced * 8) {
				fprintf(stderr, "EDIPlayer: ignored deti TAG item with wrong length (%zu bits)\n", tag_len);
				continue;
			}

			// FCT (frame count, 0..249) - low byte of the detiHeader; the high 5 bits of
			// tag_value[0] (below ATSTF/FICF/RFUDF) hold FCTH, extending it to a 0..4999 counter,
			// not needed here
			if(atstf) {
				// ATST - see EDIPlayer::DecodeFrame for the byte layout/decode reference
				const uint8_t *atst = tag_value + 2 + 4;
				uint8_t utco = atst[0];
				uint32_t seconds = (uint32_t) atst[1] << 24 | atst[2] << 16 | atst[3] << 8 | atst[4];
				uint32_t tsta = (uint32_t) atst[5] << 16 | atst[6] << 8 | atst[7];

				static const time_t edi_epoch_2000 = 946684800;
				time_t unix_seconds = edi_epoch_2000 + (time_t) seconds - utco;

				std::chrono::system_clock::time_point frame_time =
						std::chrono::system_clock::from_time_t(unix_seconds) +
						std::chrono::microseconds(std::lround(tsta / 16.384));

				// this frame only ever reaches here once the playback thread has already slept
				// until (or found itself past) this exact instant - so this is "how late are we
				// actually playing this", which should settle close to 0; see DecodeFrame for the
				// true network-arrival margin, measured on the other side of the admission queue
				std::chrono::system_clock::time_point sys_now = std::chrono::system_clock::now();
				double buffer_delay_ms = std::chrono::duration<double, std::milli>(sys_now - frame_time).count();
				int fct = tag_value[1];

				EDI_TIMING timing;
				timing.valid = true;
				timing.fct = fct;
				{
					std::lock_guard<std::mutex> lock(timing_mutex);
					last_buffer_delay_ms = buffer_delay_ms;
					// TSTA's fractional-second value is only meaningful as "tist_at_fct0" right
					// at FCT 0 - on every other FCT it is just that frame's own fractional second,
					// which must not overwrite the fixed value observed at FCT 0
					if(fct == 0)
						last_tist_at_fct0_ms = tsta / 16384.0;
					timing.margin_delay_ms = last_margin_delay_ms;
					timing.buffer_delay_ms = buffer_delay_ms;
					timing.tist_at_fct0_ms = last_tist_at_fct0_ms;
				}
				ReportEDIPlayback(timing);
			}

			if(fic_len)
				ProcessFIC(tag_value + 2 + 4 + (atstf ? 8 : 0), fic_len);

			continue;
		}

		// ETI Sub-Channel Stream
		if(tag_name.substr(0, 3) == "est" && tag_item[3] >= 1 && tag_item[3] <= 64) {
			if(tag_len < 3 * 8) {
				fprintf(stderr, "EDIPlayer: ignored est<n> TAG item with too short length (%zu bits)\n", tag_len);
				continue;
			}

			int subchid = tag_value[0] >> 2;
			const uint8_t *subch_data = tag_value + 3;
			size_t subch_bytes = (tag_len / 8) - 3;

			// independent data_sinks (e.g. SPI) are fed regardless of the selected audio service
			{
				std::lock_guard<std::mutex> data_lock(data_sinks_mutex);
				std::map<int,SubchannelSink*>::const_iterator sink_it = data_sinks.find(subchid);
				if(sink_it != data_sinks.end() && sink_it->second)
					sink_it->second->Feed(subch_data, subch_bytes);
			}

			// lock only now, as due to the FIC, the audio service may be set
			std::lock_guard<std::mutex> lock(audio_service_mutex);

			if(subchid != audio_service.subchid)
				continue;

			dec->Feed(subch_data, subch_bytes);

			continue;
		}

		// Information
		if(tag_name == "info") {
			std::string text((const char*) tag_value, tag_len / 8);

			fprintf(stderr, "EDIPlayer: info TAG item '%s'\n", text.c_str());
			continue;
		}

		// Network Adapted Signalling Channel - ignored
		if(tag_name == "nasc")
			continue;

		// Frame Padding User Data - ignored
		if(tag_name == "frpd")
			continue;

		fprintf(stderr, "EDIPlayer: ignored unsupported TAG item '%s' (%zu bits)\n", tag_name.c_str(), tag_len);
	}
}
