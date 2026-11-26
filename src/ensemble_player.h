/*
    DABlin - capital DAB experience
    Copyright (C) 2015-2024 Stefan Pöschel

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

#ifndef ENSEMBLE_PLAYER_H_
#define ENSEMBLE_PLAYER_H_

// sdl_output.h, that includes SDL.h, must be included before <string> or it won't compile on OS X
#ifndef DABLIN_DISABLE_SDL
#include "sdl_output.h"
#endif

#include <stdio.h>
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "subchannel_sink.h"
#include "dab_decoder.h"
#include "dabplus_decoder.h"
#include "pcm_output.h"
#include "wav_output.h"
#include "tools.h"
#include "audio_spectrum_analyzer.h"


// --- AudioOutputType -----------------------------------------------------------------
enum class AudioOutputType { SDL, PCM, WAV, Untouched };


// --- AUDIO_SPECTRUM -----------------------------------------------------------------
struct AUDIO_SPECTRUM {
	int samplerate = 0;
	// one magnitude (dBFS) vector per channel, in the order the stream carries them; each has
	// AudioSpectrumAnalyzer::num_bands logarithmically spaced bands, from
	// AudioSpectrumAnalyzer::min_freq to AudioSpectrumAnalyzer::max_freq
	std::vector<std::vector<double>> channels_db;
};


// --- AUDIO_STEREO_FRAME -----------------------------------------------------------------
// one block's worth of stereo analysis data, for the goniometer and the phase correlation meter;
// only ever produced for 2-channel audio - stereo imaging is meaningless for mono
struct AUDIO_STEREO_FRAME {
	bool valid = false;	// false while the block was silent (correlation is undefined: 0/0)
	double correlation = 0.0;	// -1 (out of phase) .. +1 (in phase)
	// a decimated set of (L, R) sample pairs from this block, already rotated 45 degrees (so mono
	// content points straight "up"): x = (R-L)/sqrt(2), y = (L+R)/sqrt(2), each roughly in [-1.5, 1.5]
	std::vector<std::pair<float, float>> points;
};


// --- EDI_TIMING -----------------------------------------------------------------
// decoded from an EDI "deti" TAG item's FCT and (if present) ATST fields; only ever produced for
// EDI sources whose frames actually carry an accurate timestamp (ATSTF set)
struct EDI_TIMING {
	bool valid = false;
	int fct = -1;		// 0..249, the ETI(NI)-style frame counter
	// how far "in the past" this frame's own embedded timestamp is, at the moment it is actually
	// decoded/played (on EDIPlayer's dedicated playback thread, exactly at its due time) - i.e.
	// the current end-to-end buffering delay; settles close to 0, since a frame is only ever
	// played at precisely its own scheduled time, never early and never (avoidably) late
	double buffer_delay_ms = 0.0;
	// now (at the moment this frame was actually received off the network, before any buffering)
	// minus this frame's own schedule - negative under healthy conditions, with a magnitude that
	// is the real buffering headroom the network afforded (close to -tist_offset, net of network
	// latency); fluctuates with real network jitter. Measured for every single timestamped frame
	// as it arrives (even ones about to be dropped for already being due/late, e.g. old frames
	// replayed in a mux's TCP preroll burst on a fresh connection), so it is visible converging
	// once that initial backlog has drained
	double margin_delay_ms = 0.0;
	// the TIST field's own fractional-second part (TSTA), in ms, observed at FCT 0 - matches
	// ODR-DabMux's "tist_at_fct0" config parameter, letting that setting be verified on the wire
	double tist_at_fct0_ms = 0.0;
};


// --- EnsemblePlayerObserver -----------------------------------------------------------------
class EnsemblePlayerObserver {
public:
	virtual ~EnsemblePlayerObserver() {}

	virtual void EnsembleChangeFormat(const AUDIO_SERVICE_FORMAT& /*format*/) {}
	virtual void EnsembleProcessFIC(const uint8_t* /*data*/, size_t /*len*/) {}
	virtual void EnsembleProcessPAD(const uint8_t* /*xpad_data*/, size_t /*xpad_len*/, bool /*exact_xpad_len*/, const uint8_t* /*fpad_data*/) {}
	// one entry per audio channel (1 = mono, 2 = stereo), in the order the stream carries them
	virtual void EnsembleAudioLevel(const std::vector<double>& /*channel_dbfs*/) {}
	virtual void EnsembleAudioSpectrum(const AUDIO_SPECTRUM& /*spectrum*/) {}
	virtual void EnsembleAudioStereo(const AUDIO_STEREO_FRAME& /*stereo*/) {}
	virtual void EnsembleEDITiming(const EDI_TIMING& /*timing*/) {}
};


// --- EnsemblePlayer -----------------------------------------------------------------
class EnsemblePlayer : SubchannelSinkObserver, UntouchedStreamConsumer {
protected:
	AudioOutputType audio_output_type;
	bool disable_int_catch_up;
	EnsemblePlayerObserver *observer;

	std::chrono::steady_clock::time_point next_frame_time;

	std::mutex audio_service_mutex;
	AUDIO_SERVICE audio_service;

	SubchannelSink *dec;

	// additional, independent per-sub-channel sinks fed every frame regardless of the (single)
	// selected audio_service above - e.g. the SPI packet-mode demultiplexer for a data
	// sub-channel, which must keep receiving bytes no matter what the user is listening to.
	// Sinks are owned by the caller, not by EnsemblePlayer: Add/RemoveDataSink only register a
	// pointer to feed, matching SubchannelSink's existing non-owning use for dec via SetAudioService
	std::mutex data_sinks_mutex;
	std::map<int,SubchannelSink*> data_sinks;

	AudioOutput *out;
	int audio_channels;
	int audio_samplerate;
	AudioSpectrumAnalyzer spectrum_analyzer;

	// cleared by PrepareStop() - checked by every method below that would otherwise call into
	// the observer, so that once shutdown has begun, no NEW attempt is made to report anything
	// through it. This doesn't help a report that's already in progress (e.g. blocked writing
	// into a GTK dispatcher's pipe after the main loop has died for good), but it closes off
	// every other avenue for a thread to get stuck there during/after shutdown
	std::atomic<bool> reporting_enabled;

	// set by EDIPlayer (via ReportEDITiming) whenever a frame carries an accurate timestamp; tells
	// ProcessFrame that EDIPlayer paces itself precisely against that timestamp, so its own
	// relative, fixed 24ms-per-frame pacing must stand aside rather than fight it. Only ever
	// written from EDIPlayer's admission side (its reader thread), never from its playback thread
	bool has_edi_timing;
	// EDIPlayer reports timing from two different threads (admission, as frames are received off
	// the network - reader thread; and playback, as they are actually decoded - its own dedicated
	// thread), each roughly every 24ms in steady state, which would flood the observer at ~40Hz;
	// shared by both report paths so neither one needs its own separate throttling state
	std::mutex edi_timing_notify_mutex;
	std::chrono::steady_clock::time_point edi_timing_last_notify;
	bool EDITimingShouldNotify();
	// admission side: also flips has_edi_timing
	void ReportEDITiming(const EDI_TIMING& timing);
	// playback side: notifies only, never touches has_edi_timing (which must stay reader-thread-only)
	void ReportEDIPlayback(const EDI_TIMING& timing);

	virtual void DecodeFrame(const uint8_t *ensemble_frame) = 0;

	void FormatChange(const AUDIO_SERVICE_FORMAT& format);
	void StartAudio(int samplerate, int channels);
	void PutAudio(const uint8_t *data, size_t len);

	void ProcessFIC(const uint8_t *data, size_t len);
	void ProcessPAD(const uint8_t *xpad_data, size_t xpad_len, bool exact_xpad_len, const uint8_t *fpad_data);
	void ProcessUntouchedStream(const uint8_t* data, size_t len, size_t duration_ms);

	void AudioError(const std::string& hint);
	void AudioWarning(const std::string& hint);
	void FECInfo(int total_corr_count, bool uncorr_errors);
public:
	EnsemblePlayer(AudioOutputType audio_output_type, bool disable_int_catch_up, EnsemblePlayerObserver *observer);
	~EnsemblePlayer();

	// signals EDIPlayer's playback thread (if any) to stop, without blocking on it, and disables
	// all further observer reporting - call this as early as possible, before joining any other
	// thread (e.g. the ensemble source's reader thread): once the GTK main loop has stopped for
	// good (the window has closed), nothing is left to drain the observer's cross-thread
	// notification queue, so a thread still running and reporting through it (audio levels, EDI
	// timing, FIC changes) can block forever - leaving it unsignaled for as long as some other
	// join takes makes that far more likely to happen. A subclass override must still call this
	// base implementation
	virtual void PrepareStop() {reporting_enabled = false;}

	// drops every frame received but not decoded yet, and returns only once no frame is being
	// decoded any more - call it after the source has stopped, before resetting the FIC, so nothing
	// from the previous connection reaches the decoders afterwards. Nothing to do for a player that
	// decodes each frame as soon as it is received
	virtual void Flush() {}

	void ProcessFrame(const uint8_t *data);

	bool IsSameAudioService(const AUDIO_SERVICE& audio_service);
	void SetAudioService(const AUDIO_SERVICE& audio_service);

	// registers/unregisters a sink fed from the given sub-channel every frame, independently of
	// whatever audio_service is (or isn't) currently selected; sink is not owned, caller must
	// RemoveDataSink it before destroying it
	void AddDataSink(int subchid, SubchannelSink *sink);
	void RemoveDataSink(int subchid);

	std::string GetUntouchedStreamFileExtension() {return dec ? dec->GetUntouchedStreamFileExtension() : "";}
	void AddUntouchedStreamConsumer(UntouchedStreamConsumer* consumer) {if(dec) dec->AddUntouchedStreamConsumer(consumer);};
	void RemoveUntouchedStreamConsumer(UntouchedStreamConsumer* consumer) {if(dec) dec->RemoveUntouchedStreamConsumer(consumer);};

	void StopAudio() {if(out) out->StopAudio();}
	void SetAudioMute(bool audio_mute) {if(out) out->SetAudioMute(audio_mute);}
	void SetAudioVolume(double audio_volume) {if(out) out->SetAudioVolume(audio_volume);}
	bool HasAudioVolumeControl() {return out ? out->HasAudioVolumeControl() : false;}
};

#endif /* ENSEMBLE_PLAYER_H_ */
