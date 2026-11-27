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

#ifndef EDISCOPE_QT_THEME_H_
#define EDISCOPE_QT_THEME_H_

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>


// --- EDIScopeQtTheme -----------------------------------------------------------------
// the look of the whole application, dark or light: every colour - of the style sheet and of the
// custom-drawn widgets - comes from here, so a theme change (Apply) reaches everything. Custom-drawn
// widgets read these colours when they paint; text given a role (SetRole) is coloured by the style
// sheet; anything else is redrawn by its owner when the theme changes
namespace EDIScopeQtTheme {
	enum class Theme {DARK, LIGHT};

	// general
	extern QColor window_bg;		// main window background
	extern QColor panel_bg;			// panels
	extern QColor bar_bg;			// menu bar, status bar
	extern QColor input_bg;			// inputs, buttons, empty areas inside panels
	extern QColor border;			// panel borders, separators
	extern QColor input_border;
	extern QColor text;
	extern QColor text_strong;
	extern QColor text_secondary;
	extern QColor text_muted;
	extern QColor text_disabled;
	extern QColor accent;
	extern QColor on_accent;		// text drawn on top of the accent colour
	extern QColor ok;
	extern QColor warning;
	extern QColor error;
	extern QColor info;

	// meters
	extern QColor meter_border;
	extern QColor meter_tick;
	extern QColor meter_label;
	extern QColor meter_track;		// unfilled part of a bar
	extern QColor meter_diagonal;	// stereo meter guides
	extern QColor meter_peak;		// peak-hold marker
	extern QColor meter_points;		// stereo meter cloud
	extern QColor meter_cap;		// spectrum peak-hold caps
	extern QColor meter_channel;	// "L"/"R" labels

	// CU bar
	extern QColor cu_audio;
	extern QColor cu_data;
	extern QColor cu_data_hatch;
	extern QColor cu_outline;

	// SPI programme timeline
	extern QColor timeline_block;
	extern QColor timeline_border;
	extern QColor timeline_time;

	extern QColor slide_waiting;	// "waiting for slide" placeholder

	void Apply(QApplication& app, Theme theme);
	Theme Current();
	QString ThemeName(Theme theme);		// "dark", "light" - as saved in the settings
	Theme ThemeFromName(const QString& name);

	QFont MonoFont();

	// colours a label (or any widget) through the style sheet, so it follows theme changes by
	// itself: "muted", "message", "title", "caption", "subtitle", "note", "small", "ok", "warning",
	// "error" - an empty role resets it
	void SetRole(QWidget *widget, const char *role);

	// right-click on a table: "Copy" (the cell) and "Copy row" (tab-separated)
	void MakeTableCopyable(QTableWidget *table);
}


// --- EDIScopeQtPanel -----------------------------------------------------------------
// a rounded panel with a title strip (title on the left, optional info text on the right) and a
// body below it, as in the mockup
class EDIScopeQtPanel : public QFrame {
	Q_OBJECT
private:
	QLabel *title_label;
	QLabel *info_label;
	QVBoxLayout *body_layout;
public:
	EDIScopeQtPanel(const QString& title, QWidget *parent = nullptr);

	QVBoxLayout* BodyLayout() {return body_layout;}
	void SetTitle(const QString& title) {title_label->setText(title);}
	void SetInfo(const QString& info) {info_label->setText(info);}
	QLabel* InfoLabel() {return info_label;}
};

#endif /* EDISCOPE_QT_THEME_H_ */
