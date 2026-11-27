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

#include "ediscope_qt_margin.h"
#include "ediscope_qt_theme.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include <QPainter>
#include <QPainterPath>


// the zones of the time axis: age (ms before now) from/to, and share of the plot width
struct ZONE {
	int64_t from_age;
	int64_t to_age;
	double share;
	int64_t bucket_ms;	// 0: every sample
};
static const ZONE ZONES[] = {
	{0,                   60 * 1000,        0.2, 0},
	{60 * 1000,           30 * 60 * 1000,   0.4, 60 * 1000},
	{30 * 60 * 1000,      EDIScopeQtMarginGraph::HISTORY_MS, 0.4, 5 * 60 * 1000},
};

// samples further apart than this: a gap, the line is interrupted
static const int64_t GAP_MS = 2000;

struct BUCKET {
	int64_t start;	// steady clock ms
	double min;
	double max;
	double sum;
	size_t count;
};

// a "nice" step (1, 2 or 5 times a power of ten) for about the given number of ticks over a range
static double NiceStep(double range, int ticks) {
	double raw = range / std::max(1, ticks);
	double magnitude = std::pow(10, std::floor(std::log10(raw)));
	double fraction = raw / magnitude;
	return (fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10) * magnitude;
}


// --- EDIScopeQtMarginGraph -----------------------------------------------------------------
EDIScopeQtMarginGraph::EDIScopeQtMarginGraph(QWidget *parent) : QWidget(parent), now_ms(0), live_valid(false), live_ms(0), tist_at_fct0_ms(0) {
	setMinimumSize(700, 300);
}

void EDIScopeQtMarginGraph::SetData(const history_t& history, int64_t now_ms, bool live_valid, double live_ms, double tist_at_fct0_ms) {
	this->history = history;
	this->now_ms = now_ms;
	this->live_valid = live_valid;
	this->live_ms = live_ms;
	this->tist_at_fct0_ms = tist_at_fct0_ms;
	update();
}

void EDIScopeQtMarginGraph::paintEvent(QPaintEvent* /*event*/) {
	QPainter painter(this);
	painter.fillRect(rect(), EDIScopeQtTheme::input_bg);

	QFont mono = EDIScopeQtTheme::MonoFont();
	QFontMetrics metrics(mono);
	QFont big = mono;
	big.setPointSizeF(mono.pointSizeF() * 2.2);
	big.setBold(true);
	QFontMetrics big_metrics(big);

	// --- the live value, left of the graph ---
	const int live_width = big_metrics.horizontalAdvance("-00000 ms") + 20;
	painter.setFont(big);
	painter.setPen(live_valid ? EDIScopeQtTheme::accent : EDIScopeQtTheme::text_muted);
	QRect live_rect(0, height() / 2 - big_metrics.height(), live_width, big_metrics.height());
	painter.drawText(live_rect, Qt::AlignCenter, live_valid ? QString::number((int) std::lround(live_ms)) + " ms" : QString("—"));
	painter.setFont(mono);
	painter.setPen(EDIScopeQtTheme::text_muted);
	painter.drawText(QRect(0, live_rect.bottom() + 4, live_width, metrics.height()), Qt::AlignCenter, "margin now");
	if(live_valid)
		painter.drawText(QRect(0, live_rect.bottom() + 8 + metrics.height(), live_width, metrics.height()), Qt::AlignCenter,
				"TIST@FCT0 " + QString::number((int) std::lround(tist_at_fct0_ms)) + " ms");

	// --- plot area ---
	const int axis_width = metrics.horizontalAdvance("-00000 ms") + 10;
	const int top = metrics.height() + 12, bottom = metrics.height() + 10, right = 12;
	QRect plot(live_width + axis_width, top, std::max(50, width() - live_width - axis_width - right), std::max(50, height() - top - bottom));

	// age -> x, piecewise over the zones
	std::vector<double> zone_x;	// left x of each zone, then the right end
	double x = plot.left();
	for(const ZONE& z : ZONES) {
		zone_x.push_back(x);
		x += z.share * plot.width();
	}
	zone_x.push_back(plot.right());
	auto x_for_age = [&](int64_t age) {
		for(size_t i = 0; i < sizeof(ZONES) / sizeof(ZONES[0]); i++) {
			const ZONE& z = ZONES[i];
			if(age <= z.to_age)
				return zone_x[i] + (double) (std::max(age, z.from_age) - z.from_age) / (z.to_age - z.from_age) * (zone_x[i + 1] - zone_x[i]);
		}
		return (double) plot.right();
	};
	// age -> x within zone i only (clamped to it): a period overlapping a zone boundary is cut there
	auto x_in_zone = [&](size_t i, int64_t age) {
		const ZONE& z = ZONES[i];
		age = std::min(std::max(age, z.from_age), z.to_age);
		return zone_x[i] + (double) (age - z.from_age) / (z.to_age - z.from_age) * (zone_x[i + 1] - zone_x[i]);
	};

	// --- data: raw samples of the first zone, clock-aligned buckets of the others ---
	std::vector<std::pair<int64_t, double>> raw;
	std::vector<std::vector<BUCKET>> zone_buckets(sizeof(ZONES) / sizeof(ZONES[0]));
	double min_v = 0, max_v = 0;
	bool any = false;
	auto extend = [&](double lo, double hi) {
		min_v = any ? std::min(min_v, lo) : lo;
		max_v = any ? std::max(max_v, hi) : hi;
		any = true;
	};
	for(size_t i = 0; i < zone_buckets.size(); i++) {
		const ZONE& z = ZONES[i];
		if(!z.bucket_ms) {
			for(const std::pair<int64_t, double>& sample : history)
				if(now_ms - sample.first <= z.to_age) {
					raw.push_back(sample);
					extend(sample.second, sample.second);
				}
			continue;
		}
		// the complete periods within the zone (the current one isn't complete yet - the samples
		// of the first zone show it)
		std::map<int64_t, BUCKET> buckets;
		for(const std::pair<int64_t, double>& sample : history) {
			int64_t start = sample.first / z.bucket_ms * z.bucket_ms;
			int64_t newest_age = now_ms - (start + z.bucket_ms), oldest_age = now_ms - start;
			// complete periods overlapping the zone
			if(newest_age < 0 || oldest_age <= z.from_age || newest_age >= z.to_age)
				continue;
			BUCKET& b = buckets[start];
			if(!b.count) {
				b.start = start;
				b.min = b.max = sample.second;
				b.sum = 0;
			}
			b.min = std::min(b.min, sample.second);
			b.max = std::max(b.max, sample.second);
			b.sum += sample.second;
			b.count++;
		}
		for(const std::pair<const int64_t, BUCKET>& kv : buckets) {
			zone_buckets[i].push_back(kv.second);
			extend(kv.second.min, kv.second.max);
		}
	}

	painter.setFont(mono);
	if(!any) {
		painter.setPen(EDIScopeQtTheme::text_muted);
		painter.drawText(plot, Qt::AlignCenter, "No EDI margin yet (a live EDI source with timestamps is needed).");
		return;
	}

	// vertical scale: the values shown, with a bit of room
	double pad = std::max(5.0, (max_v - min_v) * 0.1);
	min_v -= pad;
	max_v += pad;
	double step = NiceStep(max_v - min_v, 5);
	min_v = std::floor(min_v / step) * step;
	max_v = std::ceil(max_v / step) * step;
	auto y_for = [&](double v) {return plot.bottom() - (v - min_v) / (max_v - min_v) * plot.height();};

	// grid, value labels
	for(double v = min_v; v <= max_v + step / 2; v += step) {
		double y = y_for(v);
		painter.setPen(EDIScopeQtTheme::border);
		painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
		painter.setPen(EDIScopeQtTheme::text_muted);
		painter.drawText(QRectF(live_width, y - metrics.height() / 2.0, axis_width - 6, metrics.height()), Qt::AlignRight | Qt::AlignVCenter,
				QString::number((int) std::lround(v)) + " ms");
	}

	// time ticks; the zone boundaries stronger, with what each zone shows above it
	struct TICK {int64_t age; QString label;};
	std::vector<TICK> ticks = {
		{0, "now"}, {30 * 1000, "-30 s"}, {60 * 1000, "-1 min"}, {10 * 60 * 1000, "-10 min"}, {20 * 60 * 1000, "-20 min"},
		{30 * 60 * 1000, "-30 min"}, {60 * 60 * 1000, "-1 h"}, {120 * 60 * 1000, "-2 h"}, {180 * 60 * 1000, "-3 h"},
	};
	// the lines; then their labels, the zone boundaries' first, the others where there is room left
	auto is_boundary = [](int64_t age) {return age == 0 || age == 60 * 1000 || age == 30 * 60 * 1000 || age == EDIScopeQtMarginGraph::HISTORY_MS;};
	for(const TICK& t : ticks) {
		double tx = x_for_age(t.age);
		bool boundary = is_boundary(t.age);
		painter.setPen(QPen(boundary ? EDIScopeQtTheme::text_muted : EDIScopeQtTheme::border, 1, boundary ? Qt::SolidLine : Qt::DotLine));
		painter.drawLine(QPointF(tx, plot.top()), QPointF(tx, plot.bottom()));
	}
	std::vector<QRectF> drawn;
	painter.setPen(EDIScopeQtTheme::text_muted);
	for(bool boundaries : {true, false})
		for(const TICK& t : ticks) {
			if(is_boundary(t.age) != boundaries)
				continue;
			double tx = x_for_age(t.age);
			double label_width = metrics.horizontalAdvance(t.label);
			QRectF label_rect(tx - label_width / 2, plot.bottom() + 4, label_width, metrics.height());
			if(label_rect.right() > width() - 2)
				label_rect.moveRight(width() - 2);
			bool overlaps = false;
			for(const QRectF& r : drawn)
				overlaps |= label_rect.adjusted(-6, 0, 6, 0).intersects(r);
			if(overlaps)
				continue;
			painter.drawText(label_rect, Qt::AlignCenter, t.label);
			drawn.push_back(label_rect);
		}
	const char *zone_titles[] = {"every value", "1 min averages", "5 min averages"};
	for(size_t i = 0; i < zone_buckets.size(); i++) {
		QRectF title_rect(zone_x[i], 2, zone_x[i + 1] - zone_x[i], metrics.height() + 4);
		painter.drawText(title_rect, Qt::AlignCenter, metrics.elidedText(zone_titles[i], Qt::ElideRight, (int) title_rect.width() - 4));
	}

	// 0 ms, when within the scale
	if(min_v < 0 && max_v > 0) {
		painter.setPen(QPen(EDIScopeQtTheme::text_secondary, 1, Qt::DashLine));
		painter.drawLine(QPointF(plot.left(), y_for(0)), QPointF(plot.right(), y_for(0)));
	}

	painter.setRenderHint(QPainter::Antialiasing, true);
	QColor band = EDIScopeQtTheme::accent;
	band.setAlpha(70);

	// --- averaged zones: min-max band per period, the averages joined ---
	for(size_t i = 1; i < zone_buckets.size(); i++) {
		const ZONE& z = ZONES[i];
		QPainterPath line;
		int64_t prev_start = 0;
		bool first = true;
		for(const BUCKET& b : zone_buckets[i]) {
			double x_old = x_in_zone(i, now_ms - b.start);	// older side: right
			double x_new = x_in_zone(i, now_ms - (b.start + z.bucket_ms));
			QRectF band_rect(QPointF(std::min(x_new, x_old), y_for(b.max)), QPointF(std::max(x_new, x_old), y_for(b.min)));
			painter.fillRect(band_rect.adjusted(0.5, 0, -0.5, 0), band);

			QPointF p((x_new + x_old) / 2, y_for(b.sum / b.count));
			if(first || b.start - prev_start > z.bucket_ms)
				line.moveTo(p);
			else
				line.lineTo(p);
			prev_start = b.start;
			first = false;
		}
		painter.setPen(QPen(EDIScopeQtTheme::accent, 1.6));
		painter.drawPath(line);
	}

	// --- last minute: every sample, interrupted at gaps ---
	QPainterPath path;
	int64_t prev_t = 0;
	for(size_t i = 0; i < raw.size(); i++) {
		QPointF p(x_for_age(now_ms - raw[i].first), y_for(raw[i].second));
		if(i == 0 || raw[i].first - prev_t > GAP_MS)
			path.moveTo(p);
		else
			path.lineTo(p);
		prev_t = raw[i].first;
	}
	painter.setPen(QPen(EDIScopeQtTheme::accent, 1.4));
	painter.drawPath(path);

	painter.setRenderHint(QPainter::Antialiasing, false);
	painter.setPen(EDIScopeQtTheme::border);
	painter.setBrush(Qt::NoBrush);
	painter.drawRect(plot);
}
