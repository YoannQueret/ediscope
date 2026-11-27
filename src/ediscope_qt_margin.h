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

#ifndef EDISCOPE_QT_MARGIN_H_
#define EDISCOPE_QT_MARGIN_H_

#include <deque>
#include <utility>

#include <QWidget>


// --- EDIScopeQtMarginGraph -----------------------------------------------------------------
// the EDI margin delay over time (see EDI_TIMING::margin_delay_ms), from now (left) to 3 hours ago
// (right), on a time axis detailed on the left and condensed on the right:
// - the live value, as text, left of the graph;
// - the last minute: every sample;
// - 1 to 30 minutes ago: one average per minute;
// - 30 minutes to 3 hours ago: one average per 5 minutes;
// each average with the band between the minimum and maximum of its period (so a short peak still
// shows). The periods are aligned on the clock, so a complete one no longer changes
class EDIScopeQtMarginGraph : public QWidget {
	Q_OBJECT
public:
	typedef std::deque<std::pair<int64_t, double>> history_t;	// (steady clock ms, margin ms), oldest first
	static const int64_t HISTORY_MS = 3 * 3600 * 1000;		// kept, and shown
private:
	history_t history;
	int64_t now_ms;
	bool live_valid;
	double live_ms;
	double tist_at_fct0_ms;
protected:
	void paintEvent(QPaintEvent *event) override;
public:
	EDIScopeQtMarginGraph(QWidget *parent = nullptr);
	void SetData(const history_t& history, int64_t now_ms, bool live_valid, double live_ms, double tist_at_fct0_ms);
};

#endif /* EDISCOPE_QT_MARGIN_H_ */
