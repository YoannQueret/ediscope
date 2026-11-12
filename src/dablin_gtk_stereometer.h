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

#ifndef DABLIN_GTK_STEREOMETER_H_
#define DABLIN_GTK_STEREOMETER_H_

#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

#include <gtkmm.h>


// --- DABlinGTKStereoMeter -----------------------------------------------------------------
// goniometer (stereo vectorscope): plots a short persistence cloud of (L,R) sample pairs, already
// rotated 45 degrees by the caller so that mono (L=R) content points straight up ("M"),
// left-only content points to the upper-left ("L") and right-only content to the upper-right ("R")
class DABlinGTKStereoMeter : public Gtk::DrawingArea {
private:
	struct Point {
		float x, y;
	};

	static const size_t max_points = 600;
	std::deque<Point> points;

	bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
public:
	DABlinGTKStereoMeter();

	// x/y are already rotated: (R-L)/sqrt(2), (L+R)/sqrt(2), each roughly within [-1.5, 1.5]
	void AddPoints(const std::vector<std::pair<float, float>>& new_points);
	void Reset();
};

#endif /* DABLIN_GTK_STEREOMETER_H_ */
