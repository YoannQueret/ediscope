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

#include "ediscope_gtk_spectrum.h"

#include "audio_spectrum_analyzer.h"

#include <algorithm>
#include <cmath>

static const int SCALE_WIDTH = 30;
static const int FREQ_SCALE_HEIGHT = 16;
// matches FREQ_SCALE_HEIGHT, so there is as much empty space above the plot's top (0dBFS) as below
// its bottom (the frequency axis) - same margin the level meter uses at its own bottom
static const int TOP_MARGIN = 16;
static const int PLOT_MARGIN_RIGHT = 4;
static const std::chrono::seconds PEAK_HOLD_DURATION{2};
static const int NUM_BARS = 48;
static const int BAR_GAP = 1;


// --- EDIScopeGTKSpectrum -----------------------------------------------------------------
// out-of-class definitions, as odr-used (e.g. passed by reference into std::min/std::max) in C++11
constexpr double EDIScopeGTKSpectrum::min_dbfs;
constexpr double EDIScopeGTKSpectrum::max_dbfs;

static_assert(AudioSpectrumAnalyzer::num_bands % NUM_BARS == 0, "NUM_BARS must evenly divide the analyzer's band count");

EDIScopeGTKSpectrum::EDIScopeGTKSpectrum() : menu_item_bubble(mode_group, "Bubble"), menu_item_bar(mode_group, "Bar") {
	has_data = false;
	display_mode = DisplayMode::Bar;

	set_size_request(SCALE_WIDTH + 500, -1);
	set_vexpand(true);
	set_tooltip_text("Right-click to switch between the \"bubble\" and \"bar\" display styles.");

	add_events(Gdk::BUTTON_PRESS_MASK);
	menu_item_bar.set_active(true);
	menu_item_bubble.signal_toggled().connect([this]() {if(menu_item_bubble.get_active()) SetDisplayMode(DisplayMode::Bubble);});
	menu_item_bar.signal_toggled().connect([this]() {if(menu_item_bar.get_active()) SetDisplayMode(DisplayMode::Bar);});
	context_menu.append(menu_item_bubble);
	context_menu.append(menu_item_bar);
	context_menu.show_all();

	SetChannels(1);
}

void EDIScopeGTKSpectrum::SetDisplayMode(DisplayMode mode) {
	display_mode = mode;
	queue_draw();
}

bool EDIScopeGTKSpectrum::on_button_press_event(GdkEventButton* event) {
	if(event->type != GDK_BUTTON_PRESS || event->button != 3)
		return false;

	context_menu.popup_at_pointer((GdkEvent*) event);
	return true;
}

void EDIScopeGTKSpectrum::SetChannels(size_t channels) {
	channels = std::max((size_t) 1, channels);
	if(channels == bands_db.size())
		return;

	auto now = std::chrono::steady_clock::now();
	bands_db.assign(channels, std::vector<double>(AudioSpectrumAnalyzer::num_bands, min_dbfs));
	peak_db.assign(channels, std::vector<double>(AudioSpectrumAnalyzer::num_bands, min_dbfs));
	peak_time.assign(channels, std::vector<std::chrono::steady_clock::time_point>(AudioSpectrumAnalyzer::num_bands, now));
	queue_draw();
}

void EDIScopeGTKSpectrum::SetSpectrum(const AUDIO_SPECTRUM& spectrum) {
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

	queue_draw();
}

void EDIScopeGTKSpectrum::Reset() {
	auto now = std::chrono::steady_clock::now();
	for(std::vector<double>& v : bands_db)
		std::fill(v.begin(), v.end(), min_dbfs);
	for(std::vector<double>& v : peak_db)
		std::fill(v.begin(), v.end(), min_dbfs);
	for(std::vector<std::chrono::steady_clock::time_point>& v : peak_time)
		std::fill(v.begin(), v.end(), now);
	has_data = false;
	queue_draw();
}

double EDIScopeGTKSpectrum::LevelToFraction(double dbfs) {
	double t = (max_dbfs - dbfs) / (max_dbfs - min_dbfs);	// 0 at 0dBFS, 1 at the floor, linear
	double t_nonlinear = std::sqrt(std::min(std::max(t, 0.0), 1.0));	// square-root scale: expands the top, compresses the bottom
	return 1.0 - t_nonlinear;	// back to "fraction from the floor"
}

double EDIScopeGTKSpectrum::FreqToFraction(double freq_hz) {
	double f = std::min(std::max(freq_hz, AudioSpectrumAnalyzer::min_freq), AudioSpectrumAnalyzer::max_freq);
	return std::log10(f / AudioSpectrumAnalyzer::min_freq) / std::log10(AudioSpectrumAnalyzer::max_freq / AudioSpectrumAnalyzer::min_freq);
}

void EDIScopeGTKSpectrum::DrawAmplitudeScale(const Cairo::RefPtr<Cairo::Context>& cr, double baseline_y, double height, bool grow_upward, double margin_before, double margin_after) {
	static const double ticks[] = {0, -6, -12, -20, -40, -60, -90};

	Pango::FontDescription small_font = get_pango_context()->get_font_description();
	small_font.set_size(small_font.get_size() * 0.65);

	for(double tick : ticks) {
		if(tick < min_dbfs || tick > max_dbfs)
			continue;

		// the floor sits exactly on the shared center axis in mirrored (stereo) mode; only draw it
		// once, from the upward (left channel) half, to avoid stacking an identical label on itself
		if(tick == min_dbfs && !grow_upward)
			continue;

		double offset = height * LevelToFraction(tick);
		double y = grow_upward ? baseline_y - offset : baseline_y + offset;

		cr->set_source_rgb(0.4, 0.4, 0.4);
		cr->set_line_width(1);
		cr->move_to(SCALE_WIDTH - 4, y);
		cr->line_to(SCALE_WIDTH, y);
		cr->stroke();

		Glib::RefPtr<Pango::Layout> label = create_pango_layout(std::to_string((int) tick));
		label->set_font_description(small_font);
		int label_width, label_height;
		label->get_pixel_size(label_width, label_height);

		// keep the label within this half's own [top, bottom] extent, widened by margin_before/
		// margin_after wherever that extent is an outer widget edge with empty space beyond it
		// (rather than the shared center axis, where another channel's own content begins) - so
		// e.g. the "0" label can sit centered exactly on its tick instead of being pushed clear of it
		double extent_top = (grow_upward ? baseline_y - height : baseline_y) - margin_before;
		double extent_bottom = (grow_upward ? baseline_y : baseline_y + height) + margin_after;
		double label_y = std::min(std::max(y - label_height / 2.0, extent_top), extent_bottom - label_height);

		cr->set_source_rgb(0.6, 0.6, 0.6);
		cr->move_to(SCALE_WIDTH - 6 - label_width, label_y);
		label->show_in_cairo_context(cr);
	}
}

void EDIScopeGTKSpectrum::DrawFrequencyScale(const Cairo::RefPtr<Cairo::Context>& cr, double x, double width, double y) {
	// standard logarithmic audio scale: major ticks (decades + their midpoint) get a label,
	// minor ticks in between are tick marks only, to avoid cluttering the narrow plot width
	struct Tick { double freq; const char *label; };
	static const Tick ticks[] = {
		{20, "20"}, {50, nullptr}, {100, "100"}, {200, nullptr}, {500, nullptr},
		{1000, "1k"}, {2000, nullptr}, {5000, nullptr}, {10000, "10k"}, {20000, "20k"},
	};

	Pango::FontDescription small_font = get_pango_context()->get_font_description();
	small_font.set_size(small_font.get_size() * 0.65);

	for(const Tick& tick : ticks) {
		double tick_x = x + FreqToFraction(tick.freq) * width;

		cr->set_source_rgb(0.4, 0.4, 0.4);
		cr->set_line_width(1);
		cr->move_to(tick_x, y);
		cr->line_to(tick_x, y + (tick.label ? 4 : 2));
		cr->stroke();

		if(!tick.label)
			continue;

		Glib::RefPtr<Pango::Layout> label = create_pango_layout(tick.label);
		label->set_font_description(small_font);
		int label_width, label_height;
		label->get_pixel_size(label_width, label_height);

		// left/rightmost ticks are aligned inward, so their labels never spill out of the widget
		double label_x = tick_x - label_width / 2.0;
		if(tick.freq <= AudioSpectrumAnalyzer::min_freq)
			label_x = tick_x;
		else if(tick.freq >= AudioSpectrumAnalyzer::max_freq)
			label_x = tick_x - label_width;

		cr->set_source_rgb(0.6, 0.6, 0.6);
		cr->move_to(label_x, y + 5);
		label->show_in_cairo_context(cr);
	}
}

void EDIScopeGTKSpectrum::DrawChannel(const Cairo::RefPtr<Cairo::Context>& cr, size_t ch, double x, double width, double baseline_y, double height, bool grow_upward) {
	if(ch >= bands_db.size() || bands_db[ch].empty() || !has_data)
		return;

	// the bands already arrive evenly spaced on the logarithmic frequency axis (min_freq to
	// max_freq), so a band's x position is simply linear in its index
	size_t num_bands = bands_db[ch].size();
	auto band_x = [&](size_t b) {return x + (double) b / (num_bands - 1) * width;};
	auto y_for = [&](double dbfs) {
		double offset = height * LevelToFraction(dbfs);
		return grow_upward ? baseline_y - offset : baseline_y + offset;
	};

	// instantaneous spectrum: a semi-transparent filled curve
	cr->move_to(band_x(0), baseline_y);
	for(size_t b = 0; b < num_bands; b++)
		cr->line_to(band_x(b), y_for(bands_db[ch][b]));
	cr->line_to(band_x(num_bands - 1), baseline_y);
	cr->close_path();
	cr->set_source_rgba(0.3, 0.55, 0.85, 0.45);
	cr->fill_preserve();
	cr->set_source_rgba(0.55, 0.78, 1.0, 0.8);
	cr->set_line_width(1.3);
	cr->stroke();

	// 2 second peak-hold curve, overlaid thinner but brighter than the instantaneous curve
	cr->move_to(band_x(0), y_for(peak_db[ch][0]));
	for(size_t b = 1; b < num_bands; b++)
		cr->line_to(band_x(b), y_for(peak_db[ch][b]));
	cr->set_source_rgb(1.0, 1.0, 1.0);
	cr->set_line_width(0.8);
	cr->stroke();
}

// classic 1/3-octave SPL meter look: coarser gradient-colored bars (grouping several of the
// analyzer's bands into each one), with a short, bright peak-hold cap on top of each bar
void EDIScopeGTKSpectrum::DrawChannelBar(const Cairo::RefPtr<Cairo::Context>& cr, size_t ch, double x, double width, double baseline_y, double height, bool grow_upward) {
	if(ch >= bands_db.size() || bands_db[ch].empty() || !has_data)
		return;

	size_t num_bands = bands_db[ch].size();
	size_t group_size = num_bands / NUM_BARS;
	double bar_step = width / NUM_BARS;
	double bar_width = std::max(1.0, bar_step - BAR_GAP);

	auto y_for = [&](double dbfs) {
		double offset = height * LevelToFraction(dbfs);
		return grow_upward ? baseline_y - offset : baseline_y + offset;
	};

	// fixed background gradient (floor = blue, mid = green, 0dBFS = yellow), revealed up to each
	// bar's current level - same "clip to current fill, then paint a fixed scale" technique as
	// EDIScopeGTKLevelMeter's colored zones, just as a smooth gradient instead of flat color bands
	double y0 = baseline_y;
	double y1 = grow_upward ? baseline_y - height : baseline_y + height;
	Cairo::RefPtr<Cairo::LinearGradient> gradient = Cairo::LinearGradient::create(0, y0, 0, y1);
	gradient->add_color_stop_rgb(0.0, 0.15, 0.35, 0.55);
	gradient->add_color_stop_rgb(0.5, 0.25, 0.75, 0.35);
	gradient->add_color_stop_rgb(1.0, 0.95, 0.85, 0.2);

	for(int i = 0; i < NUM_BARS; i++) {
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
		cr->set_source_rgb(0.15, 0.15, 0.15);
		cr->rectangle(bar_x, grow_upward ? baseline_y - height : baseline_y, bar_width, height);
		cr->fill();

		cr->save();
		cr->rectangle(bar_x, grow_upward ? value_y : baseline_y, bar_width, grow_upward ? baseline_y - value_y : value_y - baseline_y);
		cr->clip();
		cr->set_source(gradient);
		cr->rectangle(bar_x, grow_upward ? baseline_y - height : baseline_y, bar_width, height);
		cr->fill();
		cr->restore();

		// peak-hold cap: a short, bright segment at the 2 second peak level
		cr->set_source_rgb(1.0, 0.95, 0.4);
		cr->rectangle(bar_x, peak_y - (grow_upward ? 1.5 : 0.0), bar_width, 1.5);
		cr->fill();
	}
}

bool EDIScopeGTKSpectrum::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
	Gtk::Allocation allocation = get_allocation();
	const double width = allocation.get_width();
	const double height = allocation.get_height();

	const double plot_x = SCALE_WIDTH;
	const double plot_width = std::max(0.0, width - SCALE_WIDTH - PLOT_MARGIN_RIGHT);
	const double plot_height = std::max(0.0, height - TOP_MARGIN - FREQ_SCALE_HEIGHT);

	// fill the whole widget, not just the plot area, so the frequency/amplitude scale legends sit
	// on the same dark background as the plot itself, rather than on the widget's default background
	cr->set_source_rgb(0.08, 0.08, 0.08);
	cr->paint();

	// shifts all plot content (but not the background fill above, or the border below) down by
	// TOP_MARGIN, so it isn't flush against the widget's top edge
	cr->save();
	cr->translate(0, TOP_MARGIN);

	auto draw_channel = [&](size_t ch, double baseline_y, double h, bool grow_upward) {
		if(display_mode == DisplayMode::Bubble)
			DrawChannel(cr, ch, plot_x, plot_width, baseline_y, h, grow_upward);
		else
			DrawChannelBar(cr, ch, plot_x, plot_width, baseline_y, h, grow_upward);
	};

	size_t channels = bands_db.size();
	if(channels >= 2) {
		double half = plot_height / 2.0;

		DrawAmplitudeScale(cr, half, half, true, TOP_MARGIN, 0);
		DrawAmplitudeScale(cr, half, half, false, 0, FREQ_SCALE_HEIGHT);
		draw_channel(0, half, half, true);
		draw_channel(1, half, half, false);

		// center axis, shared floor of both mirrored channels
		cr->set_source_rgb(0.3, 0.3, 0.3);
		cr->set_line_width(1);
		cr->move_to(plot_x, half + 0.5);
		cr->line_to(plot_x + plot_width, half + 0.5);
		cr->stroke();

		// channel labels, so it's clear which mirrored half is which: "L" top-right, "R" bottom-right
		Pango::FontDescription label_font = get_pango_context()->get_font_description();
		label_font.set_weight(Pango::WEIGHT_BOLD);

		auto draw_corner_label = [&](const char *text, bool top) {
			Glib::RefPtr<Pango::Layout> label = create_pango_layout(text);
			label->set_font_description(label_font);
			int label_width, label_height;
			label->get_pixel_size(label_width, label_height);

			cr->set_source_rgba(0.7, 0.7, 0.7, 0.9);
			cr->move_to(plot_x + plot_width - label_width - 4, top ? 2 : plot_height - label_height - 2);
			label->show_in_cairo_context(cr);
		};

		draw_corner_label("L", true);
		draw_corner_label("R", false);
	} else {
		DrawAmplitudeScale(cr, plot_height, plot_height, true, TOP_MARGIN, FREQ_SCALE_HEIGHT);
		draw_channel(0, plot_height, plot_height, true);
	}

	DrawFrequencyScale(cr, plot_x, plot_width, plot_height);

	cr->restore();

	// border around the whole widget, now that its background fills the whole widget too
	cr->set_source_rgb(0.3, 0.3, 0.3);
	cr->set_line_width(1);
	cr->rectangle(0.5, 0.5, width - 1, height - 1);
	cr->stroke();

	return true;
}
