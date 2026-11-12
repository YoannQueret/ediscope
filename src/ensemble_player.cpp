/*
    DABlin - capital DAB experience
    Copyright (C) 2015-2024 Stefan Pöschel

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

#include "ensemble_player.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>


// --- EnsemblePlayer -----------------------------------------------------------------
EnsemblePlayer::EnsemblePlayer(AudioOutputType audio_output_type, bool disable_int_catch_up, EnsemblePlayerObserver *observer) {
	this->audio_output_type = audio_output_type;
	this->disable_int_catch_up = disable_int_catch_up;
	this->observer = observer;

	dec = nullptr;
	out = nullptr;
	audio_channels = 1;
	audio_samplerate = 0;
	has_edi_timing = false;

	switch(audio_output_type) {
#ifndef DABLIN_DISABLE_SDL
	case AudioOutputType::SDL:
		out = new SDLOutput;
		break;
#endif
	case AudioOutputType::PCM:
		out = new PCMOutput;
		break;
	case AudioOutputType::WAV:
		out = new WAVOutput;
		break;
	case AudioOutputType::Untouched:
		break;
	default:
		throw std::runtime_error("Unsupported audio output type!");
	}
}

EnsemblePlayer::~EnsemblePlayer() {
	delete dec;
	delete out;
}

bool EnsemblePlayer::IsSameAudioService(const AUDIO_SERVICE& audio_service) {
	std::lock_guard<std::mutex> lock(audio_service_mutex);

	// check, if already the same service
	return this->audio_service == audio_service;
}

void EnsemblePlayer::SetAudioService(const AUDIO_SERVICE& audio_service) {
	std::lock_guard<std::mutex> lock(audio_service_mutex);

	// abort, if already the same service
	if(this->audio_service == audio_service)
		return;

	// cleanup - audio only stopped (externally) on ensemble change!
	if(dec) {
		delete dec;
		dec = nullptr;
	}

	if(audio_service.IsNone())
		fprintf(stderr, "EnsemblePlayer: playing nothing\n");
	else
		fprintf(stderr, "EnsemblePlayer: playing sub-channel %d (%s)\n", audio_service.subchid, audio_service.dab_plus ? "DAB+" : "DAB");

	// apply
	if(!audio_service.IsNone()) {
		if(audio_service.dab_plus)
			dec = new SuperframeFilter(this, audio_output_type != AudioOutputType::Untouched);
		else
			dec = new MP2Decoder(this);
		if(audio_output_type == AudioOutputType::Untouched)
			dec->AddUntouchedStreamConsumer(this);
	}

	this->audio_service = audio_service;
}

void EnsemblePlayer::ProcessFrame(const uint8_t *data) {
	bool init = next_frame_time.time_since_epoch().count() == 0;
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

	if(has_edi_timing) {
		// EDIPlayer paces itself precisely against each frame's own embedded timestamp, on its own
		// dedicated playback thread, fully decoupled from this call (which is on the reader thread
		// and must never block, so as to keep timestamping new frames against their true network
		// arrival time) - so this relative, fixed 24ms-per-frame clock must just stand aside
		next_frame_time = now;
	} else if(init || (disable_int_catch_up && now > next_frame_time + std::chrono::milliseconds(24))) {
		// resync, if desired and noticeable after expected arrival of next frame
		if(!init)
			fprintf(stderr, "EnsemblePlayer: resynced to stream\n");
		next_frame_time = now;
	} else {
		// otherwise just sleep until expected arrival (if not yet reached)
		std::this_thread::sleep_until(next_frame_time);
	}
	next_frame_time += std::chrono::milliseconds(24);

	DecodeFrame(data);
}

bool EnsemblePlayer::EDITimingShouldNotify() {
	static const std::chrono::milliseconds min_interval(250);
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

	std::lock_guard<std::mutex> lock(edi_timing_notify_mutex);
	if(edi_timing_last_notify.time_since_epoch().count() != 0 && now - edi_timing_last_notify < min_interval)
		return false;

	edi_timing_last_notify = now;
	return true;
}

void EnsemblePlayer::ReportEDITiming(const EDI_TIMING& timing) {
	has_edi_timing = timing.valid;

	if(observer && EDITimingShouldNotify())
		observer->EnsembleEDITiming(timing);
}

void EnsemblePlayer::ReportEDIPlayback(const EDI_TIMING& timing) {
	if(observer && EDITimingShouldNotify())
		observer->EnsembleEDITiming(timing);
}

void EnsemblePlayer::StartAudio(int samplerate, int channels) {
	audio_channels = channels;
	audio_samplerate = samplerate;
	spectrum_analyzer.Reset();
	if(out)
		out->StartAudio(samplerate, channels);
}

void EnsemblePlayer::PutAudio(const uint8_t *data, size_t len) {
	if(out)
		out->PutAudio(data, len);

	// derive a per-channel dBFS peak level from the signed 16bit interleaved PCM samples, for the level meter
	if(observer) {
		const int16_t *samples = (const int16_t*) data;
		size_t num_samples = len / sizeof(int16_t);
		int channels = audio_channels > 0 ? audio_channels : 1;

		std::vector<int16_t> peaks(channels, 0);
		for(size_t i = 0; i < num_samples; i++) {
			int16_t sample = samples[i];
			int16_t abs_sample = sample == INT16_MIN ? INT16_MAX : (int16_t) std::abs(sample);
			int16_t& peak = peaks[i % channels];
			if(abs_sample > peak)
				peak = abs_sample;
		}

		static const double silence_dbfs = -60.0;
		std::vector<double> levels_dbfs(channels);
		for(int ch = 0; ch < channels; ch++) {
			double dbfs = peaks[ch] > 0 ? 20.0 * log10(peaks[ch] / 32768.0) : silence_dbfs;
			levels_dbfs[ch] = std::max(dbfs, silence_dbfs);
		}
		observer->EnsembleAudioLevel(levels_dbfs);

		// derive the magnitude spectrum per channel too, for the spectrum analyzer widget; only
		// emitted once the analyzer's sliding window has seen enough samples to produce a result
		AUDIO_SPECTRUM spectrum;
		spectrum.channels_db = spectrum_analyzer.Process(samples, num_samples, channels, audio_samplerate);
		if(!spectrum.channels_db.empty() && !spectrum.channels_db[0].empty()) {
			spectrum.samplerate = audio_samplerate;
			observer->EnsembleAudioSpectrum(spectrum);
		}

		// derive the L/R phase correlation and a decimated, rotated sample cloud for the goniometer;
		// stereo imaging only makes sense for actual stereo audio
		if(channels == 2) {
			size_t num_frames = num_samples / 2;

			AUDIO_STEREO_FRAME stereo;
			static const size_t target_points = 100;
			size_t stride = std::max((size_t) 1, num_frames / target_points);

			double sum_lr = 0.0, sum_l2 = 0.0, sum_r2 = 0.0;
			for(size_t i = 0; i < num_frames; i++) {
				double l = samples[i * 2] / 32768.0;
				double r = samples[i * 2 + 1] / 32768.0;
				sum_lr += l * r;
				sum_l2 += l * l;
				sum_r2 += r * r;

				if(i % stride == 0)
					stereo.points.emplace_back((float) ((r - l) / std::sqrt(2.0)), (float) ((l + r) / std::sqrt(2.0)));
			}

			if(sum_l2 > 0.0 && sum_r2 > 0.0) {
				stereo.correlation = sum_lr / std::sqrt(sum_l2 * sum_r2);
				stereo.valid = true;
			}

			observer->EnsembleAudioStereo(stereo);
		}
	}
}

void EnsemblePlayer::FormatChange(const AUDIO_SERVICE_FORMAT& format) {
	fprintf(stderr, "EnsemblePlayer: format: %s\n", format.GetSummary().c_str());
	if(observer)
		observer->EnsembleChangeFormat(format);
}

void EnsemblePlayer::ProcessFIC(const uint8_t *data, size_t len) {
//	fprintf(stderr, "Received %zu bytes FIC\n", len);
	if(observer)
		observer->EnsembleProcessFIC(data, len);
}

void EnsemblePlayer::ProcessPAD(const uint8_t *xpad_data, size_t xpad_len, bool exact_xpad_len, const uint8_t *fpad_data) {
//	fprintf(stderr, "Received %zu bytes X-PAD\n", xpad_len);
	if(observer)
		observer->EnsembleProcessPAD(xpad_data, xpad_len, exact_xpad_len, fpad_data);
}

void EnsemblePlayer::ProcessUntouchedStream(const uint8_t* data, size_t len, size_t /*duration_ms*/) {
	if(audio_output_type == AudioOutputType::Untouched) {
		if(fwrite(data, len, 1, stdout) != 1)
			perror("EnsemblePlayer: error while writing untouched stream to stdout");
	}
}

void EnsemblePlayer::AudioError(const std::string& hint) {
	fprintf(stderr, "\x1B[31m" "(%s)" "\x1B[0m" " ", hint.c_str());
}

void EnsemblePlayer::AudioWarning(const std::string& hint) {
	fprintf(stderr, "\x1B[35m" "(%s)" "\x1B[0m" " ", hint.c_str());
}

void EnsemblePlayer::FECInfo(int total_corr_count, bool uncorr_errors) {
	fprintf(stderr, "\x1B[36m" "(%d%s)" "\x1B[0m" " ", total_corr_count, uncorr_errors ? "+" : "");
}
