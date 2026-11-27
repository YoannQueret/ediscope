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

#include "ediscope_qt_meters.h"

#include "audio_spectrum_analyzer.h"
#include "ediscope_qt_theme.h"

#include <algorithm>
#include <cmath>

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>


// shared by every meter, for a consistent look across the meters row - no background of their own:
// they are drawn straight onto their panel
static const int TOP_MARGIN = 16;
static const int BOTTOM_MARGIN = 16;
static const std::chrono::seconds PEAK_HOLD_DURATION{2};

static QFont SmallFont(const QWidget *widget, double factor) {
	QFont font = widget->font();
	font.setPointSizeF(font.pointSizeF() * factor);
	return font;
}


// --- EDIScopeQtLevelMeter -----------------------------------------------------------------
static const int LEVEL_SCALE_WIDTH = 28;
static const int LEVEL_BAR_GAP = 3;
static const int LEVEL_MAX_CHANNELS = 2;	// DAB audio is mono or stereo at most
static const int LEVEL_BARS_AREA_WIDTH = LEVEL_MAX_CHANNELS * 16 + (LEVEL_MAX_CHANNELS - 1) * LEVEL_BAR_GAP;

constexpr double EDIScopeQtLevelMeter::min_dbfs;
constexpr double EDIScopeQtLevelMeter::max_dbfs;

EDIScopeQtLevelMeter::EDIScopeQtLevelMeter(QWidget *parent) : QWidget(parent) {
	// fixed width regardless of channel count, so mono/stereo switches never resize the widget:
	// in mono, the single bar simply spans the full area the two stereo bars would occupy
	setFixedWidth(LEVEL_SCALE_WIDTH + LEVEL_BARS_AREA_WIDTH + 8);
	setMinimumHeight(200);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	setToolTip("Audio level (dBFS), with the peak of the last 2 seconds");
	SetChannels(1);
}

void EDIScopeQtLevelMeter::SetChannels(size_t channels) {
	channels = std::max((size_t) 1, channels);
	if(channels == levels_dbfs.size())
		return;

	levels_dbfs.assign(channels, min_dbfs);
	peak_dbfs.assign(channels, min_dbfs);
	peak_time.assign(channels, std::chrono::steady_clock::now());
	update();
}

void EDIScopeQtLevelMeter::SetLevels(const std::vector<double>& dbfs) {
	if(dbfs.size() != levels_dbfs.size())
		SetChannels(dbfs.size());

	auto now = std::chrono::steady_clock::now();
	bool changed = false;

	for(size_t i = 0; i < levels_dbfs.size() && i < dbfs.size(); i++) {
		double clamped = std::min(std::max(dbfs[i], min_dbfs), max_dbfs);
		if(clamped != levels_dbfs[i]) {
			levels_dbfs[i] = clamped;
			changed = true;
		}

		// new peak, or the held one is older than the hold duration: (re)start the hold from here
		if(clamped >= peak_dbfs[i] || now - peak_time[i] > PEAK_HOLD_DURATION) {
			if(clamped != peak_dbfs[i])
				changed = true;
			peak_dbfs[i] = clamped;
			peak_time[i] = now;
		}
	}

	if(changed)
		update();
}

void EDIScopeQtLevelMeter::Reset() {
	auto now = std::chrono::steady_clock::now();
	std::fill(levels_dbfs.begin(), levels_dbfs.end(), min_dbfs);
	std::fill(peak_dbfs.begin(), peak_dbfs.end(), min_dbfs);
	std::fill(peak_time.begin(), peak_time.end(), now);
	update();
}

double EDIScopeQtLevelMeter::LevelToFraction(double dbfs) {
	double t = (max_dbfs - dbfs) / (max_dbfs - min_dbfs);	// 0 at 0dBFS (top), 1 at the floor (bottom), linear
	double t_nonlinear = std::sqrt(std::min(std::max(t, 0.0), 1.0));	// square-root scale: expands the top, compresses the bottom
	return 1.0 - t_nonlinear;	// back to "fraction from the bottom"
}

void EDIScopeQtLevelMeter::paintEvent(QPaintEvent* /*event*/) {
	QPainter painter(this);

	const double height = std::max(0.0, (double) this->height() - TOP_MARGIN - BOTTOM_MARGIN);
	auto y_from_top = [&](double d) {return TOP_MARGIN + height - height * LevelToFraction(d);};

	// dB scale (closer spacing near 0dBFS reflects the non-linear bar scale)
	static const double ticks[] = {0, -3, -6, -12, -20, -40, -60};
	QFont small_font = SmallFont(this, 0.75);
	painter.setFont(small_font);
	QFontMetricsF metrics(small_font);
	for(double tick : ticks) {
		double y = y_from_top(tick);
		painter.setPen(EDIScopeQtTheme::meter_tick);
		painter.drawLine(QPointF(LEVEL_SCALE_WIDTH - 4, y), QPointF(LEVEL_SCALE_WIDTH, y));

		QString label = QString::number((int) tick);
		double label_height = metrics.height();
		painter.setPen(EDIScopeQtTheme::meter_label);
		painter.drawText(QRectF(0, y - label_height / 2, LEVEL_SCALE_WIDTH - 6, label_height), Qt::AlignRight | Qt::AlignVCenter, label);
	}

	// one bar per channel (left first); whatever the channel count, the bars always fill the same total width
	size_t channels = levels_dbfs.size();
	double bar_width = (LEVEL_BARS_AREA_WIDTH - (double) (channels - 1) * LEVEL_BAR_GAP) / channels;
	for(size_t ch = 0; ch < channels; ch++) {
		double x = LEVEL_SCALE_WIDTH + 2 + ch * (bar_width + LEVEL_BAR_GAP);
		auto px_from_bottom = [&](double d) {return height * LevelToFraction(d);};
		double bottom = TOP_MARGIN + height;

		// background represents the unfilled part of the bar
		painter.fillRect(QRectF(x, TOP_MARGIN, bar_width, height), EDIScopeQtTheme::meter_track);

		// colour zones (broadcast VU meter convention), revealed up to the current level
		painter.save();
		painter.setClipRect(QRectF(x, bottom - px_from_bottom(levels_dbfs[ch]), bar_width, px_from_bottom(levels_dbfs[ch])));
		painter.fillRect(QRectF(x, bottom - px_from_bottom(-12.0), bar_width, px_from_bottom(-12.0)), QColor("#40bf40"));
		painter.fillRect(QRectF(x, bottom - px_from_bottom(-3.0), bar_width, px_from_bottom(-3.0) - px_from_bottom(-12.0)), QColor("#e6cc26"));
		painter.fillRect(QRectF(x, TOP_MARGIN, bar_width, height - px_from_bottom(-3.0)), QColor("#d93333"));
		painter.restore();

		// peak-hold marker: highest level reached in the last 2 seconds
		double peak_y = bottom - px_from_bottom(peak_dbfs[ch]);
		painter.fillRect(QRectF(x, std::max(peak_y - 1, (double) TOP_MARGIN), bar_width, 2), EDIScopeQtTheme::meter_peak);

		painter.setPen(EDIScopeQtTheme::meter_border);
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(QRectF(x + 0.5, TOP_MARGIN + 0.5, bar_width - 1, height - 1));
	}
}


// --- EDIScopeQtStereoMeter -----------------------------------------------------------------
static const int STEREO_METER_SIZE = 200;
// plotted points reach up to sqrt(2) at full-scale mono/single-channel content; add some headroom
static const double STEREO_PLOT_RANGE = 1.6;

EDIScopeQtStereoMeter::EDIScopeQtStereoMeter(QWidget *parent) : QWidget(parent) {
	setMinimumSize(STEREO_METER_SIZE, STEREO_METER_SIZE);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	setFixedWidth(STEREO_METER_SIZE);
	setToolTip(
			"Stereo vectorscope (goniometer)\n"
			"Mono content points straight up (M), left/right-only content points to the upper-left/upper-right (L/R)");
}

void EDIScopeQtStereoMeter::AddPoints(const std::vector<std::pair<float, float>>& new_points) {
	if(new_points.empty())
		return;

	for(const std::pair<float, float>& p : new_points)
		points.push_back(p);
	while(points.size() > max_points)
		points.pop_front();

	update();
}

void EDIScopeQtStereoMeter::Reset() {
	points.clear();
	update();
}

void EDIScopeQtStereoMeter::paintEvent(QPaintEvent* /*event*/) {
	QPainter painter(this);

	// a square plot, centered within whatever (possibly taller) height the row gives this widget
	const double size = std::min(width(), height());
	painter.translate((width() - size) / 2.0, (height() - size) / 2.0);

	const double cx = size / 2.0;
	const double cy = size / 2.0;
	const double scale = size / 2.0 / STEREO_PLOT_RANGE;

	// corner-to-corner dashed diagonals
	QPen dash_pen(EDIScopeQtTheme::meter_diagonal, 1, Qt::DashLine);
	painter.setPen(dash_pen);
	painter.drawLine(QPointF(0, 0), QPointF(size, size));
	painter.drawLine(QPointF(size, 0), QPointF(0, size));

	QFont small_font = SmallFont(this, 0.8);
	painter.setFont(small_font);
	painter.setPen(EDIScopeQtTheme::meter_label);
	auto draw_label = [&](const char *text, double x, double y) {
		painter.drawText(QRectF(x - 10, y - 8, 20, 16), Qt::AlignCenter, text);
	};
	draw_label("L", 10, 10);
	draw_label("M", cx, 10);
	draw_label("R", size - 10, 10);

	// persistence cloud: oldest points first, so newer ones are painted on top and more opaque
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setPen(Qt::NoPen);
	size_t n = points.size();
	for(size_t i = 0; i < n; i++) {
		double alpha = 0.15 + 0.85 * ((double) i / std::max((size_t) 1, n - 1));
		double px = cx + points[i].first * scale;
		double py = cy - points[i].second * scale;

		QColor point = EDIScopeQtTheme::meter_points;
		point.setAlphaF(alpha);
		painter.setBrush(point);
		painter.drawEllipse(QPointF(px, py), 1.3, 1.3);
	}
	painter.setRenderHint(QPainter::Antialiasing, false);
}


// --- EDIScopeQtCorrelationMeter -----------------------------------------------------------------
static const int CORRELATION_LEFT_LABEL_WIDTH = 26;	// correlation value labels (as wide as the angle labels, so the bar sits in the middle)
static const int CORRELATION_RIGHT_LABEL_WIDTH = 26;	// equivalent phase angle labels
static const int CORRELATION_BAR_WIDTH = 24;

EDIScopeQtCorrelationMeter::EDIScopeQtCorrelationMeter(QWidget *parent) : QWidget(parent), has_data(false), correlation(0.0) {
	setFixedWidth(CORRELATION_LEFT_LABEL_WIDTH + CORRELATION_BAR_WIDTH + CORRELATION_RIGHT_LABEL_WIDTH);
	setMinimumHeight(200);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	setToolTip(
			"Stereo phase correlation (left: coefficient, right: phase angle)\n"
			"+1 = in phase (mono-compatible), 0 = uncorrelated, -1 = out of phase (mono-incompatible)");
}

void EDIScopeQtCorrelationMeter::SetCorrelation(double correlation) {
	this->correlation = std::min(std::max(correlation, -1.0), 1.0);
	has_data = true;
	update();
}

void EDIScopeQtCorrelationMeter::Reset() {
	has_data = false;
	correlation = 0.0;
	update();
}

void EDIScopeQtCorrelationMeter::paintEvent(QPaintEvent* /*event*/) {
	struct Tick {double corr; const char *label;};
	static const Tick ticks[] = {
		{1.0, "+1"}, {0.5, "+.5"}, {0.0, "0"}, {-0.5, "-.5"}, {-1.0, "-1"},
	};

	QPainter painter(this);

	const double bar_x = CORRELATION_LEFT_LABEL_WIDTH;
	const double bar_width = CORRELATION_BAR_WIDTH;
	const double scale_y = TOP_MARGIN;
	const double scale_height = std::max(0.0, (double) height() - TOP_MARGIN - BOTTOM_MARGIN);

	// +1 (in phase) at the top, -1 (out of phase) at the bottom - same "up is good" convention as the level meter
	auto y_for = [&](double corr) {return scale_y + (1.0 - (corr + 1.0) / 2.0) * scale_height;};

	QFont small_font = SmallFont(this, 0.75);
	painter.setFont(small_font);
	QFontMetricsF metrics(small_font);
	double label_height = metrics.height();

	for(const Tick& tick : ticks) {
		double y = y_for(tick.corr);

		painter.setPen(EDIScopeQtTheme::meter_tick);
		painter.drawLine(QPointF(bar_x, y), QPointF(bar_x + bar_width, y));

		painter.setPen(EDIScopeQtTheme::meter_label);
		painter.drawText(QRectF(0, y - label_height / 2, bar_x - 3, label_height), Qt::AlignRight | Qt::AlignVCenter, tick.label);

		// equivalent phase angle: 0deg at +1 (in phase), 90deg at 0 (uncorrelated), 180deg at -1 (out of phase)
		int degrees = (int) std::lround(std::acos(tick.corr) * 180.0 / M_PI);
		painter.drawText(QRectF(bar_x + bar_width + 3, y - label_height / 2, CORRELATION_RIGHT_LABEL_WIDTH, label_height), Qt::AlignLeft | Qt::AlignVCenter, QString::number(degrees) + "°");
	}

	if(has_data) {
		double y = y_for(correlation);

		QColor color;
		if(correlation >= -0.2)
			color = QColor("#40cc4c");	// well correlated / mono-compatible
		else if(correlation >= -0.6)
			color = QColor("#e6cc26");	// borderline
		else
			color = QColor("#e63333");	// out of phase / mono-incompatible
		painter.fillRect(QRectF(bar_x + 1, y - 1.5, bar_width - 2, 3), color);
	}

	painter.setPen(EDIScopeQtTheme::meter_border);
	painter.setBrush(Qt::NoBrush);
	painter.drawRect(QRectF(bar_x + 0.5, scale_y + 0.5, bar_width - 1, scale_height - 1));
}


// --- EDIScopeQtSpectrum -----------------------------------------------------------------
static const int SPECTRUM_SCALE_WIDTH = 32;
static const int SPECTRUM_FREQ_SCALE_HEIGHT = BOTTOM_MARGIN;
static const int SPECTRUM_PLOT_MARGIN_RIGHT = 4;
// the number of bars follows the width: the most of these (each dividing the analyzer's band count
// evenly) that keeps every bar at least SPECTRUM_MIN_BAR_STEP pixels wide - 48 at the least
static constexpr int SPECTRUM_BAR_COUNTS[] = {48, 64, 96, 128, 192, 384};
static const double SPECTRUM_MIN_BAR_STEP = 7.0;
static const int SPECTRUM_BAR_GAP = 1;

constexpr double EDIScopeQtSpectrum::min_dbfs;
constexpr double EDIScopeQtSpectrum::max_dbfs;

static constexpr bool BarCountsDivideBands() {
	for(int count : SPECTRUM_BAR_COUNTS)
		if(AudioSpectrumAnalyzer::num_bands % count)
			return false;
	return true;
}
static_assert(BarCountsDivideBands(), "every SPECTRUM_BAR_COUNTS entry must evenly divide the analyzer's band count");

EDIScopeQtSpectrum::EDIScopeQtSpectrum(QWidget *parent) : QWidget(parent), has_data(false), display_mode(DisplayMode::Bar) {
	setMinimumSize(SPECTRUM_SCALE_WIDTH + 400, 200);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setToolTip("Right-click to switch between the \"bubble\" and \"bar\" display styles.");
	SetChannels(1);
}

void EDIScopeQtSpectrum::contextMenuEvent(QContextMenuEvent *event) {
	QMenu menu(this);
	QActionGroup group(&menu);
	QAction *bubble = menu.addAction("Bubble");
	QAction *bar = menu.addAction("Bar");
	for(QAction *action : {bubble, bar}) {
		action->setCheckable(true);
		group.addAction(action);
	}
	(display_mode == DisplayMode::Bubble ? bubble : bar)->setChecked(true);

	QAction *chosen = menu.exec(event->globalPos());
	if(chosen == bubble)
		display_mode = DisplayMode::Bubble;
	else if(chosen == bar)
		display_mode = DisplayMode::Bar;
	update();
}

void EDIScopeQtSpectrum::SetChannels(size_t channels) {
	channels = std::max((size_t) 1, channels);
	if(channels == bands_db.size())
		return;

	auto now = std::chrono::steady_clock::now();
	bands_db.assign(channels, std::vector<double>(AudioSpectrumAnalyzer::num_bands, min_dbfs));
	peak_db.assign(channels, std::vector<double>(AudioSpectrumAnalyzer::num_bands, min_dbfs));
	peak_time.assign(channels, std::vector<std::chrono::steady_clock::time_point>(AudioSpectrumAnalyzer::num_bands, now));
	update();
}

void EDIScopeQtSpectrum::SetSpectrum(const AUDIO_SPECTRUM& spectrum) {
	if(spectrum.channels_db.empty())
		return;

	if(spectrum.channels_db.size() != bands_db.size())
		SetChannels(spectrum.channels_db.size());

	has_data = true;

	auto now = std::chrono::steady_clock::now();
	for(size_t ch = 0; ch < bands_db.size() && ch < spectrum.channels_db.size(); ch++) {
		const std::vector<double>& in = spectrum.channels_db[ch];
		for(size_t b = 0; b < bands_db[ch].size() && b < in.size(); b++) {
			double clamped = std::min(std::max(in[b], min_dbfs), max_dbfs);
			bands_db[ch][b] = clamped;

			// new peak, or the held one is older than the hold duration: (re)start the hold from here
			if(clamped >= peak_db[ch][b] || now - peak_time[ch][b] > PEAK_HOLD_DURATION) {
				peak_db[ch][b] = clamped;
				peak_time[ch][b] = now;
			}
		}
	}

	update();
}

void EDIScopeQtSpectrum::Reset() {
	auto now = std::chrono::steady_clock::now();
	for(std::vector<double>& v : bands_db)
		std::fill(v.begin(), v.end(), min_dbfs);
	for(std::vector<double>& v : peak_db)
		std::fill(v.begin(), v.end(), min_dbfs);
	for(std::vector<std::chrono::steady_clock::time_point>& v : peak_time)
		std::fill(v.begin(), v.end(), now);
	has_data = false;
	update();
}

double EDIScopeQtSpectrum::LevelToFraction(double dbfs) {
	double t = (max_dbfs - dbfs) / (max_dbfs - min_dbfs);	// 0 at 0dBFS, 1 at the floor, linear
	double t_nonlinear = std::sqrt(std::min(std::max(t, 0.0), 1.0));	// square-root scale: expands the top, compresses the bottom
	return 1.0 - t_nonlinear;	// back to "fraction from the floor"
}

double EDIScopeQtSpectrum::FreqToFraction(double freq_hz) {
	double f = std::min(std::max(freq_hz, AudioSpectrumAnalyzer::min_freq), AudioSpectrumAnalyzer::max_freq);
	return std::log10(f / AudioSpectrumAnalyzer::min_freq) / std::log10(AudioSpectrumAnalyzer::max_freq / AudioSpectrumAnalyzer::min_freq);
}

void EDIScopeQtSpectrum::DrawAmplitudeScale(QPainter& painter, double baseline_y, double height, bool grow_upward, double margin_before, double margin_after) {
	static const double ticks[] = {0, -6, -12, -20, -40, -60, -90};

	QFontMetricsF metrics(painter.font());
	double label_height = metrics.height();

	for(double tick : ticks) {
		// the floor sits exactly on the shared center axis in mirrored (stereo) mode; only draw it
		// once, from the upward (left channel) half
		if(tick == min_dbfs && !grow_upward)
			continue;

		double offset = height * LevelToFraction(tick);
		double y = grow_upward ? baseline_y - offset : baseline_y + offset;

		painter.setPen(EDIScopeQtTheme::meter_tick);
		painter.drawLine(QPointF(SPECTRUM_SCALE_WIDTH - 4, y), QPointF(SPECTRUM_SCALE_WIDTH, y));

		// keep the label within this half's own extent, widened by the outer margins
		double extent_top = (grow_upward ? baseline_y - height : baseline_y) - margin_before;
		double extent_bottom = (grow_upward ? baseline_y : baseline_y + height) + margin_after;
		double label_y = std::min(std::max(y - label_height / 2.0, extent_top), extent_bottom - label_height);

		painter.setPen(EDIScopeQtTheme::meter_label);
		painter.drawText(QRectF(0, label_y, SPECTRUM_SCALE_WIDTH - 6, label_height), Qt::AlignRight | Qt::AlignVCenter, QString::number((int) tick));
	}
}

void EDIScopeQtSpectrum::DrawFrequencyScale(QPainter& painter, double x, double width, double y) {
	// major ticks (decades + their midpoint) get a label, minor ticks in between are tick marks only
	struct Tick {double freq; const char *label;};
	static const Tick ticks[] = {
		{20, "20"}, {50, nullptr}, {100, "100"}, {200, nullptr}, {500, nullptr},
		{1000, "1k"}, {2000, nullptr}, {5000, nullptr}, {10000, "10k"}, {20000, "20k"},
	};

	QFontMetricsF metrics(painter.font());

	for(const Tick& tick : ticks) {
		double tick_x = x + FreqToFraction(tick.freq) * width;

		painter.setPen(EDIScopeQtTheme::meter_tick);
		painter.drawLine(QPointF(tick_x, y), QPointF(tick_x, y + (tick.label ? 4 : 2)));

		if(!tick.label)
			continue;

		double label_width = metrics.horizontalAdvance(tick.label);

		// left/rightmost ticks are aligned inward, so their labels never spill out of the widget
		double label_x = tick_x - label_width / 2.0;
		if(tick.freq <= AudioSpectrumAnalyzer::min_freq)
			label_x = tick_x;
		else if(tick.freq >= AudioSpectrumAnalyzer::max_freq)
			label_x = tick_x - label_width;

		painter.setPen(EDIScopeQtTheme::meter_label);
		painter.drawText(QPointF(label_x, y + 4 + metrics.ascent()), tick.label);
	}
}

void EDIScopeQtSpectrum::DrawChannelBubble(QPainter& painter, size_t ch, double x, double width, double baseline_y, double height, bool grow_upward) {
	if(ch >= bands_db.size() || bands_db[ch].empty() || !has_data)
		return;

	// the bands arrive evenly spaced on the logarithmic frequency axis, so a band's x position is linear in its index
	size_t num_bands = bands_db[ch].size();
	auto band_x = [&](size_t b) {return x + (double) b / (num_bands - 1) * width;};
	auto y_for = [&](double dbfs) {
		double offset = height * LevelToFraction(dbfs);
		return grow_upward ? baseline_y - offset : baseline_y + offset;
	};

	painter.setRenderHint(QPainter::Antialiasing, true);

	// instantaneous spectrum: a semi-transparent filled curve
	QPainterPath curve;
	curve.moveTo(band_x(0), baseline_y);
	for(size_t b = 0; b < num_bands; b++)
		curve.lineTo(band_x(b), y_for(bands_db[ch][b]));
	curve.lineTo(band_x(num_bands - 1), baseline_y);
	curve.closeSubpath();
	painter.fillPath(curve, QColor::fromRgbF(0.3, 0.55, 0.85, 0.45));
	painter.strokePath(curve, QPen(QColor::fromRgbF(0.55, 0.78, 1.0, 0.8), 1.3));

	// 2 second peak-hold curve, thinner but brighter than the instantaneous curve
	QPainterPath peak;
	peak.moveTo(band_x(0), y_for(peak_db[ch][0]));
	for(size_t b = 1; b < num_bands; b++)
		peak.lineTo(band_x(b), y_for(peak_db[ch][b]));
	painter.strokePath(peak, QPen(EDIScopeQtTheme::meter_peak, 0.8));

	painter.setRenderHint(QPainter::Antialiasing, false);
}

void EDIScopeQtSpectrum::DrawChannelBar(QPainter& painter, size_t ch, double x, double width, double baseline_y, double height, bool grow_upward) {
	if(ch >= bands_db.size() || bands_db[ch].empty() || !has_data)
		return;

	size_t num_bands = bands_db[ch].size();
	int num_bars = SPECTRUM_BAR_COUNTS[0];
	for(int count : SPECTRUM_BAR_COUNTS)
		if(width / count >= SPECTRUM_MIN_BAR_STEP && (size_t) count <= num_bands)
			num_bars = count;
	size_t group_size = num_bands / num_bars;
	double bar_step = width / num_bars;
	double bar_width = std::max(1.0, bar_step - SPECTRUM_BAR_GAP);

	auto y_for = [&](double dbfs) {
		double offset = height * LevelToFraction(dbfs);
		return grow_upward ? baseline_y - offset : baseline_y + offset;
	};

	// fixed gradient (floor = blue, mid = green, 0dBFS = yellow), revealed up to each bar's current level
	double y0 = baseline_y;
	double y1 = grow_upward ? baseline_y - height : baseline_y + height;
	QLinearGradient gradient(0, y0, 0, y1);
	gradient.setColorAt(0.0, QColor::fromRgbF(0.15, 0.35, 0.55));
	gradient.setColorAt(0.5, QColor::fromRgbF(0.25, 0.75, 0.35));
	gradient.setColorAt(1.0, QColor::fromRgbF(0.95, 0.85, 0.2));

	double area_top = grow_upward ? baseline_y - height : baseline_y;

	for(int i = 0; i < num_bars; i++) {
		double bar_x = x + i * bar_step;

		double value = min_dbfs;
		double peak = min_dbfs;
		for(size_t b = i * group_size; b < (size_t) (i + 1) * group_size; b++) {
			value = std::max(value, bands_db[ch][b]);
			peak = std::max(peak, peak_db[ch][b]);
		}

		double value_y = y_for(value);
		double peak_y = y_for(peak);

		// background represents the unfilled part of the bar
		painter.fillRect(QRectF(bar_x, area_top, bar_width, height), EDIScopeQtTheme::meter_track);

		// filled part
		QRectF fill = grow_upward ? QRectF(bar_x, value_y, bar_width, baseline_y - value_y) : QRectF(bar_x, baseline_y, bar_width, value_y - baseline_y);
		painter.fillRect(fill, gradient);

		// peak-hold cap: a short, bright segment at the 2 second peak level
		painter.fillRect(QRectF(bar_x, peak_y - (grow_upward ? 1.5 : 0.0), bar_width, 1.5), EDIScopeQtTheme::meter_cap);
	}
}

void EDIScopeQtSpectrum::paintEvent(QPaintEvent* /*event*/) {
	QPainter painter(this);

	const double plot_x = SPECTRUM_SCALE_WIDTH;
	const double plot_width = std::max(0.0, (double) width() - SPECTRUM_SCALE_WIDTH - SPECTRUM_PLOT_MARGIN_RIGHT);
	const double plot_height = std::max(0.0, (double) height() - TOP_MARGIN - SPECTRUM_FREQ_SCALE_HEIGHT);

	QFont small_font = SmallFont(this, 0.75);
	painter.setFont(small_font);

	painter.translate(0, TOP_MARGIN);

	auto draw_channel = [&](size_t ch, double baseline_y, double h, bool grow_upward) {
		if(display_mode == DisplayMode::Bubble)
			DrawChannelBubble(painter, ch, plot_x, plot_width, baseline_y, h, grow_upward);
		else
			DrawChannelBar(painter, ch, plot_x, plot_width, baseline_y, h, grow_upward);
	};

	size_t channels = bands_db.size();
	if(channels >= 2) {
		double half = plot_height / 2.0;

		DrawAmplitudeScale(painter, half, half, true, TOP_MARGIN, 0);
		DrawAmplitudeScale(painter, half, half, false, 0, SPECTRUM_FREQ_SCALE_HEIGHT);
		draw_channel(0, half, half, true);
		draw_channel(1, half, half, false);

		// center axis, shared floor of both mirrored channels
		painter.setPen(EDIScopeQtTheme::meter_border);
		painter.drawLine(QPointF(plot_x, half + 0.5), QPointF(plot_x + plot_width, half + 0.5));

		// channel labels, so it's clear which mirrored half is which: "L" top-right, "R" bottom-right
		QFont label_font = font();
		label_font.setBold(true);
		painter.setFont(label_font);
		painter.setPen(EDIScopeQtTheme::meter_channel);
		QFontMetricsF label_metrics(label_font);
		painter.drawText(QRectF(plot_x, 2, plot_width - 4, label_metrics.height()), Qt::AlignRight | Qt::AlignTop, "L");
		painter.drawText(QRectF(plot_x, plot_height - label_metrics.height() - 2, plot_width - 4, label_metrics.height()), Qt::AlignRight | Qt::AlignBottom, "R");
		painter.setFont(small_font);
	} else {
		DrawAmplitudeScale(painter, plot_height, plot_height, true, TOP_MARGIN, SPECTRUM_FREQ_SCALE_HEIGHT);
		draw_channel(0, plot_height, plot_height, true);
	}

	DrawFrequencyScale(painter, plot_x, plot_width, plot_height);
}
