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
#include <QAbstractButton>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>


// --- EDIScopeQtTheme -----------------------------------------------------------------
// the look of the whole application, dark or light: every colour - of the style sheet and of the
// custom-drawn widgets - comes from here, so a theme change (Apply) reaches everything. Custom-drawn
// widgets read these colours when they paint; text given a role (SetRole) is coloured by the style
// sheet; anything else is redrawn by its owner when the theme changes
namespace EDIScopeQtTheme {
	// SYSTEM: dark or light, as the desktop's colour scheme (dark if the desktop doesn't tell)
	enum class Theme {SYSTEM, DARK, LIGHT};

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
	Theme Current();			// as chosen, SYSTEM included
	Theme CurrentEffective();	// DARK or LIGHT, as applied
	QString ThemeName(Theme theme);		// "system", "dark", "light" - as saved in the settings
	Theme ThemeFromName(const QString& name);	// SYSTEM for anything unknown (or nothing saved)

	QFont MonoFont();

	// colours a label (or any widget) through the style sheet, so it follows theme changes by
	// itself: "muted", "message", "title", "caption", "subtitle", "note", "small", "ok", "warning",
	// "error" - an empty role resets it
	void SetRole(QWidget *widget, const char *role);

	// right-click on a table: "Copy" (the cell) and "Copy row" (tab-separated)
	void MakeTableCopyable(QTableWidget *table);

	// centres a (tool) window on the main window, kept within the screen's available area (and
	// shrunk to it, if larger) - so it opens in view even on a small screen
	void CenterOnWindow(QWidget *window, QWidget *main_window);

	// right-click menu of an image file shown in a window (e.g. a logo), as the slideshow's: "Save
	// <what> as..." writes the exact, original file (not re-encoded), "Copy" the image to the
	// clipboard. suggested_name: the file's own name (or URL), completed with an extension from
	// its MIME type if it has none
	void ShowImageFileMenu(QWidget *parent, const QPoint& global_pos, const QByteArray& data, const QImage& image,
			const QString& suggested_name, const QString& mime, const QString& what);
}


// --- EDIScopeQtPanel -----------------------------------------------------------------
// a rounded panel with a title strip (title on the left, optional info text on the right, then an
// optional alert badge at the far right) and a body below it, as in the mockup
class EDIScopeQtPanel : public QFrame {
	Q_OBJECT
private:
	QWidget *title_box;		// the title and the widgets right after it (AddTitleWidget)
	QHBoxLayout *title_layout;
	QLabel *title_label;
	QLabel *info_label;
	QLabel *alert_label;
	QHBoxLayout *head_layout;
	QVBoxLayout *body_layout;
public:
	EDIScopeQtPanel(const QString& title, QWidget *parent = nullptr);

	QVBoxLayout* BodyLayout() {return body_layout;}
	void SetTitle(const QString& title) {title_label->setText(title);}
	void SetInfo(const QString& info) {info_label->setText(info);}
	QLabel* InfoLabel() {return info_label;}
	QLabel* TitleLabel() {return title_label;}
	QWidget* TitleBox() {return title_box;}
	// a badge in white on red (e.g. "⚠ ALARM"), hidden when empty
	void SetAlert(const QString& text, const QString& tooltip = QString());
	// an extra widget in the title strip, on the right, just before the info text
	void AddHeadWidget(QWidget *widget);
	// an extra widget in the title strip, right after the title
	void AddTitleWidget(QWidget *widget);
};



// --- EDIScopeQtIconButton -----------------------------------------------------------------
// a small, flat button with a drawn icon - in the theme's colours (the accent while hovered), so it
// looks the same on every system and follows theme changes - for an action next to a title or a
// value: a graph, details (ⓘ), a choice (▾)
class EDIScopeQtIconButton : public QAbstractButton {
	Q_OBJECT
public:
	enum class Icon {GRAPH, INFO, MENU};
private:
	Icon icon;
protected:
	void paintEvent(QPaintEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void leaveEvent(QEvent *event) override;
public:
	EDIScopeQtIconButton(Icon icon, const QString& tooltip, QWidget *parent = nullptr);
	// part of a block that gets the mouse instead of it (see MakeHoverBlock): highlighted while the
	// block is hovered
	static void MakeHoverBlock(QWidget *block);
	QSize sizeHint() const override {return QSize(18, 18);}
};

#endif /* EDISCOPE_QT_THEME_H_ */
