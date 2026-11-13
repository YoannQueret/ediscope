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

#ifndef DABLIN_GTK_CORRELATION_H_
#define DABLIN_GTK_CORRELATION_H_

#include <gtkmm.h>


// --- EDIScopeGTKCorrelationMeter -----------------------------------------------------------------
// phase correlation meter: a vertical -1 (bottom) to +1 (top) scale (also labeled with the
// equivalent phase angle, from 180 degrees at the bottom down to 0 degrees at the top) and a
// single bar showing the currently playing service's L/R correlation coefficient
class EDIScopeGTKCorrelationMeter : public Gtk::DrawingArea {
private:
	bool has_data;
	double correlation;	// -1..+1

	bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
public:
	EDIScopeGTKCorrelationMeter();

	void SetCorrelation(double correlation);
	void Reset();
};

#endif /* DABLIN_GTK_CORRELATION_H_ */
