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

#ifndef EDISCOPE_QT_MNSC_H_
#define EDISCOPE_QT_MNSC_H_

#include <map>
#include <vector>

#include <QDateTime>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

#include "mnsc_decoder.h"


// --- EDIScopeQtMNSCWindow -----------------------------------------------------------------
// floating window listing the Transmitter Set-up and Control information carried in the MNSC
// (ETS 300 799 annex A.3.2.1): one row per transmitter address, and every change of the values
// signalled for an address. It is fed all the time (not only while visible), so it covers the
// whole connection
class EDIScopeQtMNSCWindow : public QWidget {
	Q_OBJECT
private:
	// the main window: not the Qt parent, so that this window can go below it, and be
	// maximised/minimised on its own
	QWidget *main_window;
	struct ENTRY {
		MNSC_TSC tsc;
		unsigned long messages;
		QDateTime first_seen;
		QDateTime last_seen;
		qint64 last_interval_ms;	// between the last two messages, -1 if only one
	};
	struct CHANGE {
		QDateTime time;
		uint16_t address;
		QString field;
		QString old_value;
		QString new_value;
	};

	std::map<uint16_t, ENTRY> transmitters;
	std::vector<CHANGE> changes;
	unsigned long changes_serial;		// bumped on every change of the list
	unsigned long changes_shown_serial;	// the one the table shows
	unsigned long messages;
	unsigned long crc_errors;
	bool dirty;

	QTimer refresh_timer;
	bool positioned;

	QLabel *summary_label;
	QTableWidget *transmitters_view;
	QTableWidget *changes_view;

	void Refresh();
	void Export();
protected:
	void closeEvent(QCloseEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
public:
	EDIScopeQtMNSCWindow(QWidget *parent);

	void AddTSC(const MNSC_TSC& tsc);
	void AddCRCError();
	void Clear();

	// places the window centred on the main window - only the first time it is shown
	void AlignToParent();

	// re-colours what was coloured when filled, after a theme change
	void ApplyTheme() {dirty = true; if(isVisible()) Refresh();}

	static QString AddressString(uint16_t address);
	static QString OffsetString(int offset_delay_us);
	static QString FlagsString(uint16_t flags);
signals:
	// the user closed the window (it is only hidden)
	void Closed();
};

#endif /* EDISCOPE_QT_MNSC_H_ */
