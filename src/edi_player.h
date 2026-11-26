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

#ifndef EDI_PLAYER_H_
#define EDI_PLAYER_H_

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "ensemble_player.h"


// --- EDIPlayer -----------------------------------------------------------------
// reading off the network and actually decoding/playing a frame are deliberately split across two
// threads: DecodeFrame (below) is called synchronously by the reader thread as soon as a frame is
// complete, and must never block - it only peeks the frame's own schedule (if any), reports the
// true network-arrival margin, and either drops it outright (already due/late, e.g. a mux's TCP
// preroll burst replaying old frames on a fresh connection) or hands it to a small queue. A
// dedicated playback thread drains that queue and only then does the actual decode (FIC/audio),
// exactly at each frame's own due time - never earlier, never (avoidably) later.
class EDIPlayer : public EnsemblePlayer {
private:
	struct QueuedFrame {
		std::vector<uint8_t> data;
		std::chrono::system_clock::time_point frame_time;
		bool has_time;	// false if this frame carried no (usable) ATST - played back immediately
	};

	std::thread playback_thread;
	std::mutex queue_mutex;
	std::condition_variable queue_cv;
	std::deque<QueuedFrame> queue;
	bool do_exit;

	// bumped by Flush() (under queue_mutex): a frame taken from the queue before that is dropped
	unsigned long flush_generation;
	// held by the playback thread while it decodes a frame, so Flush() can wait for that to end
	std::mutex playback_mutex;

	// last margin/buffer delay seen, so a report from either side of the split always carries
	// both figures, even though they are now measured independently, often several frames apart
	std::mutex timing_mutex;
	double last_margin_delay_ms;
	double last_buffer_delay_ms;
	double last_tist_at_fct0_ms;

	void DecodeFrame(const uint8_t *edi_frame);
	void PlaybackThreadMain();
	void PlaybackFrame(const uint8_t *edi_frame);
public:
	EDIPlayer(AudioOutputType audio_output_type, bool disable_int_catch_up, EnsemblePlayerObserver *observer);
	~EDIPlayer();

	void PrepareStop() override;
	void Flush() override;
};

#endif /* EDI_PLAYER_H_ */
