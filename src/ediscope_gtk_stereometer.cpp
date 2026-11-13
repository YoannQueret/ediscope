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

#include "ediscope_gtk_stereometer.h"

#include <algorithm>
#include <cmath>

static const int METER_SIZE = 220;
// plotted points reach up to sqrt(2) at full-scale mono/single-channel content; add some headroom
// so such points don't sit exactly on the widget's edge
static const double PLOT_RANGE = 1.6;

EDIScopeGTKStereoMeter::EDIScopeGTKStereoMeter() {
	// minimum size only - if the meters row ends up taller (vexpand), on_draw below centers a
	// square plot within whatever (possibly non-square) allocation this actually gets; a plain
	// size request can't achieve that itself, since a horizontal Gtk::Box settles every child's
	// width before the row's final (vexpand-stretched) height is even known
	set_size_request(METER_SIZE, METER_SIZE);
	set_vexpand(true);
	set_tooltip_text(
			"Stereo vectorscope (goniometer)\n"
			"Mono content points straight up (M), left/right-only content points to the upper-left/upper-right (L/R)"
	);
}

void EDIScopeGTKStereoMeter::AddPoints(const std::vector<std::pair<float, float>>& new_points) {
	if(new_points.empty())
		return;

	for(const std::pair<float, float>& p : new_points)
		points.push_back({p.first, p.second});
	while(points.size() > max_points)
		points.pop_front();

	queue_draw();
}

void EDIScopeGTKStereoMeter::Reset() {
	points.clear();
	queue_draw();
}

bool EDIScopeGTKStereoMeter::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
	Gtk::Allocation allocation = get_allocation();
	const double full_width = allocation.get_width();
	const double full_height = allocation.get_height();

	// same background as the spectrum/level meter, for a consistent look across the meters row;
	// painted across the full allocation, so any pillarboxing/letterboxing below blends in
	cr->set_source_rgb(0.08, 0.08, 0.08);
	cr->paint();

	// center a square plot within the allocation, so the widget always looks square (width ==
	// height) even if the row ends up taller than METER_SIZE (vexpand)
	const double width = std::min(full_width, full_height);
	const double height = width;
	cr->translate((full_width - width) / 2.0, (full_height - height) / 2.0);

	const double cx = width / 2.0;
	const double cy = height / 2.0;
	const double scale = width / 2.0 / PLOT_RANGE;

	// corner-to-corner dashed diagonals
	std::vector<double> dashes = {2.0, 2.0};
	cr->set_dash(dashes, 0);
	cr->set_source_rgb(0.35, 0.35, 0.35);
	cr->set_line_width(1);
	cr->move_to(0, 0);
	cr->line_to(width, height);
	cr->stroke();
	cr->move_to(width, 0);
	cr->line_to(0, height);
	cr->stroke();
	cr->unset_dash();

	Pango::FontDescription small_font = get_pango_context()->get_font_description();
	small_font.set_size(small_font.get_size() * 0.75);

	auto draw_label = [&](const char *text, double x, double y) {
		Glib::RefPtr<Pango::Layout> label = create_pango_layout(text);
		label->set_font_description(small_font);
		int lw, lh;
		label->get_pixel_size(lw, lh);
		cr->set_source_rgb(0.6, 0.6, 0.6);
		cr->move_to(x - lw / 2.0, y - lh / 2.0);
		label->show_in_cairo_context(cr);
	};
	draw_label("L", 8, 8);
	draw_label("M", cx, 8);
	draw_label("R", width - 8, 8);

	// persistence cloud: oldest points first, so newer ones are painted on top and more opaque
	size_t n = points.size();
	for(size_t i = 0; i < n; i++) {
		double alpha = 0.15 + 0.85 * ((double) i / std::max((size_t) 1, n - 1));
		double px = cx + points[i].x * scale;
		double py = cy - points[i].y * scale;

		cr->set_source_rgba(1.0, 0.9, 0.2, alpha);
		cr->arc(px, py, 1.3, 0, 2 * M_PI);
		cr->fill();
	}

	cr->set_source_rgb(0.3, 0.3, 0.3);
	cr->set_line_width(1);
	cr->rectangle(0.5, 0.5, width - 1, height - 1);
	cr->stroke();

	return true;
}
