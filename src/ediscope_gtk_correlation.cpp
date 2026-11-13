/*
    DABlin - capital DAB experience

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

#include "ediscope_gtk_correlation.h"

#include <algorithm>
#include <cmath>

static const int METER_WIDTH = 55;
static const int METER_HEIGHT = 220;
static const int LEFT_LABEL_WIDTH = 20;	// correlation value labels
static const int RIGHT_LABEL_WIDTH = 20;	// equivalent phase angle labels
// same top/bottom margins as the spectrum, so "0" (+1, top) and "180" (-1, bottom) line up with the
// spectrum's own "0" (top) and bottom edge
static const int TOP_MARGIN = 16;
static const int BOTTOM_MARGIN = 16;

struct Tick { double corr; const char *label; };
static const Tick ticks[] = {
	{1.0, "+1"}, {0.5, "+.5"}, {0.0, "0"}, {-0.5, "-.5"}, {-1.0, "-1"},
};

EDIScopeGTKCorrelationMeter::EDIScopeGTKCorrelationMeter() {
	has_data = false;
	correlation = 0.0;

	set_size_request(METER_WIDTH, METER_HEIGHT);
	set_vexpand(true);	// fills the row's actual height, same as the spectrum/stereo meter
	set_tooltip_text(
			"Stereo phase correlation\n"
			"+1 = in phase (mono-compatible), 0 = uncorrelated, -1 = out of phase (mono-incompatible)"
	);
}

void EDIScopeGTKCorrelationMeter::SetCorrelation(double correlation) {
	this->correlation = std::min(std::max(correlation, -1.0), 1.0);
	has_data = true;
	queue_draw();
}

void EDIScopeGTKCorrelationMeter::Reset() {
	has_data = false;
	correlation = 0.0;
	queue_draw();
}

bool EDIScopeGTKCorrelationMeter::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
	Gtk::Allocation allocation = get_allocation();
	const double width = allocation.get_width();
	const double height = allocation.get_height();

	const double bar_x = LEFT_LABEL_WIDTH;
	const double bar_width = width - LEFT_LABEL_WIDTH - RIGHT_LABEL_WIDTH;
	const double scale_y = TOP_MARGIN;
	const double scale_height = std::max(0.0, height - TOP_MARGIN - BOTTOM_MARGIN);

	// +1 (in phase) at the top, -1 (out of phase) at the bottom - same "up is good" convention as
	// the level meter
	auto y_for = [&](double corr) {return scale_y + (1.0 - (corr + 1.0) / 2.0) * scale_height;};

	// same background as the spectrum/level meter and the stereo meter, for a consistent look
	// across the meters row; painted for the whole widget, not just the bar, so the label margins
	// match too
	cr->set_source_rgb(0.08, 0.08, 0.08);
	cr->paint();

	Pango::FontDescription small_font = get_pango_context()->get_font_description();
	small_font.set_size(small_font.get_size() * 0.65);

	auto draw_label = [&](const std::string& text, double x, double y) {
		Glib::RefPtr<Pango::Layout> label = create_pango_layout(text);
		label->set_font_description(small_font);
		int lw, lh;
		label->get_pixel_size(lw, lh);
		cr->move_to(x, std::min(std::max(y - lh / 2.0, 0.0), height - lh));
		label->show_in_cairo_context(cr);
	};

	for(const Tick& tick : ticks) {
		double y = y_for(tick.corr);

		cr->set_source_rgb(0.4, 0.4, 0.4);
		cr->set_line_width(1);
		cr->move_to(bar_x, y);
		cr->line_to(bar_x + bar_width, y);
		cr->stroke();

		cr->set_source_rgb(0.6, 0.6, 0.6);
		draw_label(tick.label, 2, y);

		// equivalent phase angle: 0deg at +1 (in phase), 90deg at 0 (uncorrelated), 180deg at -1 (out of phase)
		int degrees = (int) std::lround(std::acos(tick.corr) * 180.0 / M_PI);
		draw_label(std::to_string(degrees), bar_x + bar_width + 3, y);
	}

	if(has_data) {
		double y = y_for(correlation);

		double r, g, b;
		if(correlation >= -0.2) {
			r = 0.25; g = 0.8; b = 0.3;	// green: well correlated / mono-compatible
		} else if(correlation >= -0.6) {
			r = 0.9; g = 0.8; b = 0.15;	// yellow: borderline
		} else {
			r = 0.9; g = 0.2; b = 0.2;	// red: out of phase / mono-incompatible
		}
		cr->set_source_rgb(r, g, b);
		cr->rectangle(bar_x + 1, y - 1.5, bar_width - 2, 3);
		cr->fill();
	}

	cr->set_source_rgb(0.3, 0.3, 0.3);
	cr->set_line_width(1);
	cr->rectangle(bar_x + 0.5, scale_y + 0.5, bar_width - 1, scale_height - 1);
	cr->stroke();

	return true;
}
