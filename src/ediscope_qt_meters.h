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

#ifndef EDISCOPE_QT_METERS_H_
#define EDISCOPE_QT_METERS_H_

#include <chrono>
#include <deque>
#include <utility>
#include <vector>

#include <QWidget>

#include "ensemble_player.h"


// The meters share the same top/bottom margins (16px), so the 0dBFS marks of the level meter and the
// spectrum, and the +1 mark of the correlation meter, line up across the meters row.


// --- EDIScopeQtLevelMeter -----------------------------------------------------------------
// vertical dBFS peak level meter for the currently playing service's audio; one bar per channel
// (mono: 1 bar, stereo: 2 bars, left first), with a dB scale and a peak-hold marker for the
// highest level reached in the last 2 seconds
class EDIScopeQtLevelMeter : public QWidget {
	Q_OBJECT
private:
	std::vector<double> levels_dbfs;
	std::vector<double> peak_dbfs;
	std::vector<std::chrono::steady_clock::time_point> peak_time;

	void SetChannels(size_t channels);
	// maps a dBFS value to a (non-linear) fraction of the scale's height, 0 = min_dbfs (bottom), 1 = max_dbfs (top);
	// compresses the low end so there is more resolution close to 0dBFS, where level differences matter most
	static double LevelToFraction(double dbfs);
protected:
	void paintEvent(QPaintEvent *event) override;
public:
	static constexpr double min_dbfs = -60.0;
	static constexpr double max_dbfs = 0.0;

	EDIScopeQtLevelMeter(QWidget *parent = nullptr);
	void SetLevels(const std::vector<double>& dbfs);
	void Reset();
};


// --- EDIScopeQtStereoMeter -----------------------------------------------------------------
// goniometer (stereo vectorscope): plots a short persistence cloud of (L,R) sample pairs, already
// rotated 45 degrees so that mono (L=R) content points straight up ("M"), left-only content points
// to the upper-left ("L") and right-only content to the upper-right ("R")
class EDIScopeQtStereoMeter : public QWidget {
	Q_OBJECT
private:
	static const size_t max_points = 600;
	std::deque<std::pair<float, float>> points;
protected:
	void paintEvent(QPaintEvent *event) override;
public:
	EDIScopeQtStereoMeter(QWidget *parent = nullptr);
	// x/y are already rotated: (R-L)/sqrt(2), (L+R)/sqrt(2), each roughly within [-1.5, 1.5]
	void AddPoints(const std::vector<std::pair<float, float>>& new_points);
	void Reset();
};


// --- EDIScopeQtCorrelationMeter -----------------------------------------------------------------
// phase correlation meter: a vertical -1 (bottom) to +1 (top) scale (also labeled with the
// equivalent phase angle, from 180 degrees at the bottom to 0 degrees at the top) and a marker
// showing the currently playing service's L/R correlation coefficient
class EDIScopeQtCorrelationMeter : public QWidget {
	Q_OBJECT
private:
	bool has_data;
	double correlation;	// -1..+1
protected:
	void paintEvent(QPaintEvent *event) override;
public:
	EDIScopeQtCorrelationMeter(QWidget *parent = nullptr);
	void SetCorrelation(double correlation);
	void Reset();
};


// --- EDIScopeQtSpectrum -----------------------------------------------------------------
// frequency spectrum analyzer for the currently playing service's audio: a logarithmic 20Hz-20kHz
// frequency axis, a dB amplitude scale and a 2 second peak-hold overlaid on the instantaneous
// spectrum; a right-click menu switches between two display styles sharing that same scale:
// "bubble" (a semi-transparent filled curve with a thin, bright peak-hold curve) and "bar" (coarser
// gradient-coloured bars with a peak-hold cap on each, akin to a classic 1/3-octave SPL meter).
// Mono shows a single spectrum (floor at the bottom, 0dBFS at the top); stereo is mirrored around a
// common central axis (floor): the left channel above, the right channel below
class EDIScopeQtSpectrum : public QWidget {
	Q_OBJECT
private:
	enum class DisplayMode {Bubble, Bar};
	static constexpr double min_dbfs = -90.0;
	static constexpr double max_dbfs = 0.0;

	bool has_data;
	std::vector<std::vector<double>> bands_db;	// [channel][band]
	std::vector<std::vector<double>> peak_db;	// [channel][band]
	std::vector<std::vector<std::chrono::steady_clock::time_point>> peak_time;	// [channel][band]
	DisplayMode display_mode;

	void SetChannels(size_t channels);
	void DrawFrequencyScale(QPainter& painter, double x, double width, double y);
	void DrawAmplitudeScale(QPainter& painter, double baseline_y, double height, bool grow_upward, double margin_before, double margin_after);
	void DrawChannelBubble(QPainter& painter, size_t ch, double x, double width, double baseline_y, double height, bool grow_upward);
	void DrawChannelBar(QPainter& painter, size_t ch, double x, double width, double baseline_y, double height, bool grow_upward);
	static double LevelToFraction(double dbfs);
	static double FreqToFraction(double freq_hz);
protected:
	void paintEvent(QPaintEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;
public:
	EDIScopeQtSpectrum(QWidget *parent = nullptr);
	void SetSpectrum(const AUDIO_SPECTRUM& spectrum);
	void Reset();
};

#endif /* EDISCOPE_QT_METERS_H_ */
