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
// the flat dark look of the redesign mockup - every colour used by the custom-drawn widgets
// comes from here, so the whole application stays consistent
namespace EDIScopeQtTheme {
	extern const QColor window_bg;		// main window background
	extern const QColor panel_bg;		// panels
	extern const QColor bar_bg;			// menu bar, status bar
	extern const QColor input_bg;		// inputs, buttons, empty areas inside panels
	extern const QColor border;			// panel borders, separators
	extern const QColor input_border;
	extern const QColor text;
	extern const QColor text_strong;
	extern const QColor text_secondary;
	extern const QColor text_muted;
	extern const QColor text_disabled;
	extern const QColor accent;
	extern const QColor on_accent;		// text drawn on top of the accent colour
	extern const QColor ok;
	extern const QColor warning;
	extern const QColor error;
	extern const QColor info;

	void Apply(QApplication& app);
	QFont MonoFont();

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
