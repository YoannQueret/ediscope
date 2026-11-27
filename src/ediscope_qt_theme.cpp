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

#include "ediscope_qt_theme.h"

#include <cstring>
#include <utility>
#include <vector>

#include <QClipboard>
#include <QDir>
#include <QMenu>
#include <QProxyStyle>
#include <QFontDatabase>
#include <QPainter>
#include <QPixmap>
#include <QStandardPaths>
#include <QHBoxLayout>
#include <QPalette>
#include <QStyleFactory>


// --- EDIScopeQtTheme -----------------------------------------------------------------
namespace EDIScopeQtTheme {
	QColor window_bg, panel_bg, bar_bg, input_bg, border, input_border;
	QColor text, text_strong, text_secondary, text_muted, text_disabled;
	QColor accent, on_accent, ok, warning, error, info;
	QColor meter_border, meter_tick, meter_label, meter_track, meter_diagonal, meter_peak, meter_points, meter_cap, meter_channel;
	QColor cu_audio, cu_data, cu_data_hatch, cu_outline;
	QColor timeline_block, timeline_border, timeline_time;
	QColor slide_waiting;

	static Theme current_theme = Theme::DARK;

	// every colour of a theme, by the name the style sheet template uses (@name@)
	typedef std::vector<std::pair<const char*, const char*>> palette_t;

	static const palette_t dark_palette = {
		{"window_bg", "#15171b"}, {"panel_bg", "#1b1f25"}, {"bar_bg", "#121417"}, {"toolbar_bg", "#191c22"},
		{"input_bg", "#12151a"}, {"border", "#262b32"}, {"input_border", "#2b313a"}, {"hover_border", "#3d4550"},
		{"disabled_border", "#22272e"}, {"text", "#e8ebee"}, {"text_strong", "#f3f5f7"}, {"text_secondary", "#c7ccd2"},
		{"text_muted", "#9aa3ad"}, {"text_disabled", "#5c646e"}, {"input_text", "#dfe3e8"}, {"input_text_disabled", "#7b838d"},
		{"accent", "#5eead4"}, {"on_accent", "#0d1210"}, {"ok", "#34d399"}, {"warning", "#fbbf24"}, {"error", "#f87171"}, {"info", "#60a5fa"},
		{"menu_hover", "#1f242b"}, {"menu_selected", "#2a3038"}, {"alt_row", "#181c21"}, {"grid", "#21252b"},
		{"header_bg", "#15181d"}, {"header_text", "#aeb6c0"}, {"header_divider", "#1f2329"},
		{"selection_bg", "#23403c"}, {"selection_text", "#ffffff"}, {"text_selection_bg", "#2e5f59"},
		{"scroll_handle", "#3a4049"}, {"tooltip_bg", "#22272e"}, {"progress_chunk", "#2e8f80"}, {"slider_handle", "#e8ebee"},
		{"meter_border", "#4d4d4d"}, {"meter_tick", "#666666"}, {"meter_label", "#999999"}, {"meter_track", "#262626"},
		{"meter_diagonal", "#595959"}, {"meter_peak", "#ffffff"}, {"meter_points", "#ffe633"}, {"meter_cap", "#fff266"},
		{"meter_channel", "#b3b3b3"},
		{"cu_audio", "#2f5f86"}, {"cu_data", "#3a4049"}, {"cu_data_hatch", "#59616c"}, {"cu_outline", "#0f1216"},
		{"timeline_block", "#2f5f86"}, {"timeline_border", "#4a7fb0"}, {"timeline_time", "#c9dcf0"},
		{"slide_waiting", "#003000"},
	};

	// light: the same structure, every text colour dark enough on its background (at least 4.5:1),
	// the bright accents of the dark theme deepened so they stay readable on white
	static const palette_t light_palette = {
		{"window_bg", "#eef0f3"}, {"panel_bg", "#ffffff"}, {"bar_bg", "#e4e7ec"}, {"toolbar_bg", "#f6f7f9"},
		{"input_bg", "#f7f8fa"}, {"border", "#d5dae1"}, {"input_border", "#bfc6cf"}, {"hover_border", "#8d97a3"},
		{"disabled_border", "#dde1e6"}, {"text", "#1f2328"}, {"text_strong", "#0d1117"}, {"text_secondary", "#353c45"},
		{"text_muted", "#59636e"}, {"text_disabled", "#9aa3ad"}, {"input_text", "#1f2328"}, {"input_text_disabled", "#8a939d"},
		{"accent", "#0f766e"}, {"on_accent", "#ffffff"}, {"ok", "#15803d"}, {"warning", "#b45309"}, {"error", "#b91c1c"}, {"info", "#1d4ed8"},
		{"menu_hover", "#d9dee5"}, {"menu_selected", "#dbe9e7"}, {"alt_row", "#f6f8fa"}, {"grid", "#e6e9ed"},
		{"header_bg", "#eef1f4"}, {"header_text", "#353c45"}, {"header_divider", "#dde1e6"},
		{"selection_bg", "#c9ebe6"}, {"selection_text", "#0d1117"}, {"text_selection_bg", "#b4e2dc"},
		{"scroll_handle", "#c2c8d0"}, {"tooltip_bg", "#ffffff"}, {"progress_chunk", "#14968a"}, {"slider_handle", "#ffffff"},
		{"meter_border", "#a8b0ba"}, {"meter_tick", "#8b949e"}, {"meter_label", "#59636e"}, {"meter_track", "#e5e8ec"},
		{"meter_diagonal", "#b8bfc8"}, {"meter_peak", "#1f2328"}, {"meter_points", "#d9480f"}, {"meter_cap", "#c2410c"},
		{"meter_channel", "#353c45"},
		{"cu_audio", "#bcd9f2"}, {"cu_data", "#e5e7eb"}, {"cu_data_hatch", "#9ca3af"}, {"cu_outline", "#7c8591"},
		{"timeline_block", "#dbeafe"}, {"timeline_border", "#93c5fd"}, {"timeline_time", "#1e3a8a"},
		{"slide_waiting", "#d8eedd"},
	};

	static const char *style_sheet = R"(
QMainWindow, QDialog { background: @window_bg@; }
QWidget { color: @text@; }

QMenuBar { background: @bar_bg@; color: @text_secondary@; border-bottom: 1px solid @border@; padding: 2px 6px; }
QMenuBar::item { padding: 4px 10px; background: transparent; }
QMenuBar::item:selected { background: @menu_hover@; border-radius: 4px; }
QMenu { background: @panel_bg@; color: @text@; border: 1px solid @input_border@; padding: 4px; }
QMenu::item { padding: 5px 22px 5px 22px; border-radius: 4px; }
QMenu::item:selected { background: @menu_selected@; }
QMenu::item:disabled { color: @text_disabled@; }
QMenu::separator { height: 1px; background: @border@; margin: 4px 6px; }

QToolBar { background: @toolbar_bg@; border: none; border-bottom: 1px solid @border@; padding: 6px 10px; spacing: 8px; }
QToolBar::separator { background: @input_border@; width: 1px; margin: 2px 6px; }

QStatusBar { background: @bar_bg@; color: @text_secondary@; border-top: 1px solid @border@; }
QStatusBar::item { border: none; }
QStatusBar QLabel { padding: 0 8px; }

QFrame#panel { background: @panel_bg@; border: 1px solid @border@; border-radius: 10px; }
QFrame#panelHead { background: transparent; border: none; border-bottom: 1px solid @border@; }
QLabel#panelTitle { color: @text_strong@; font-weight: 600; }
QLabel#panelInfo { color: @text_muted@; }
QWidget#cuBox { border-top: 1px solid @border@; }

QPushButton, QToolButton {
	background: @input_bg@; border: 1px solid @input_border@; border-radius: 7px; color: @text@;
	padding: 5px 12px; min-height: 18px;
}
QPushButton:hover, QToolButton:hover { border-color: @hover_border@; }
QPushButton:checked, QToolButton:checked { border-color: @accent@; color: @accent@; }
QPushButton:disabled, QToolButton:disabled { color: @text_disabled@; border-color: @disabled_border@; }
QPushButton#connectButton { background: @accent@; border-color: @accent@; color: @on_accent@; font-weight: 600; padding: 5px 16px; }
QPushButton#connectButton:checked { background: @input_bg@; color: @accent@; }
QPushButton#recordButton[recording="true"] { border-color: @error@; color: @error@; font-weight: 600; }

QComboBox, QLineEdit {
	background: @input_bg@; border: 1px solid @input_border@; border-radius: 7px; color: @input_text@;
	padding: 4px 8px; selection-background-color: @text_selection_bg@; selection-color: @text_strong@;
}
QComboBox:disabled, QLineEdit:disabled { color: @input_text_disabled@; }
QComboBox::drop-down { border: none; border-left: 1px solid @input_border@; width: 26px; }
QComboBox::down-arrow { image: url(@ARROW_DOWN@); width: 10px; height: 6px; }
QComboBox::down-arrow:disabled { image: url(@ARROW_DOWN_DISABLED@); }
QComboBox QAbstractItemView { background: @panel_bg@; color: @text@; border: 1px solid @input_border@; selection-background-color: @menu_selected@; selection-color: @text_strong@; }

QCheckBox { color: @text@; spacing: 8px; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid @hover_border@; border-radius: 3px; background: @input_bg@; }
QCheckBox::indicator:checked { background: @accent@; border-color: @accent@; image: url(@CHECK_MARK@); }
QCheckBox::indicator:disabled { border-color: @disabled_border@; }
QCheckBox:disabled { color: @text_disabled@; }
QRadioButton { color: @text@; spacing: 8px; }

QTableView, QListView, QTreeView {
	background: @panel_bg@; alternate-background-color: @alt_row@; border: none; gridline-color: @grid@;
	selection-background-color: @selection_bg@; selection-color: @selection_text@; outline: 0;
}
QHeaderView::section {
	background: @header_bg@; color: @header_text@; border: none; border-bottom: 1px solid @border@;
	border-right: 1px solid @header_divider@; padding: 5px 8px;
}
QTableCornerButton::section { background: @header_bg@; border: none; }
QPlainTextEdit, QTextEdit { background: @input_bg@; color: @text@; selection-background-color: @text_selection_bg@; selection-color: @text_strong@; }
QPlainTextEdit#xmlView { border: 1px solid @border@; border-radius: 6px; }
QScrollArea { background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle { background: @scroll_handle@; border-radius: 5px; min-height: 24px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QTabWidget::pane { border: 1px solid @border@; border-radius: 8px; background: @panel_bg@; top: -1px; }
QTabBar::tab {
	background: @input_bg@; color: @text_secondary@; border: 1px solid @input_border@; border-bottom: none;
	padding: 6px 14px; margin-right: 2px; border-top-left-radius: 6px; border-top-right-radius: 6px;
}
QTabBar::tab:selected { background: @panel_bg@; color: @text_strong@; border-color: @accent@; font-weight: 600; }
QTabBar::tab:hover:!selected { border-color: @hover_border@; }
QSplitter::handle { background: @border@; }
QSplitter::handle:horizontal { width: 1px; margin: 0 4px; }

QToolTip { background: @tooltip_bg@; color: @text@; border: 1px solid @hover_border@; padding: 4px; }
QWidget#imagePopup { background: @tooltip_bg@; border: 1px solid @hover_border@; }
QWidget#imagePopup QLabel { background: transparent; border: none; color: @text@; }

QProgressBar { background: @input_bg@; border: 1px solid @input_border@; border-radius: 4px; text-align: center; color: @text_secondary@; max-height: 14px; }
QProgressBar::chunk { background: @progress_chunk@; border-radius: 3px; }

QSlider::groove:horizontal { height: 4px; background: @input_border@; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent@; border-radius: 2px; }
QSlider::handle:horizontal { background: @slider_handle@; border: 1px solid @hover_border@; width: 12px; height: 12px; margin: -5px 0; border-radius: 7px; }
QSlider::handle:horizontal:disabled { background: @text_disabled@; }

QLabel#slideImage { background: @input_bg@; border-radius: 6px; }
QFrame#spiLogo { background: @input_bg@; border: 1px solid @input_border@; border-radius: 6px; }
QListWidget#logoList { background: transparent; }
QListWidget#logoList::item { color: @text_muted@; }

QLabel[role="muted"] { color: @text_muted@; }
QLabel[role="message"] { color: @text_muted@; font-size: 13px; }
QLabel[role="title"] { font-weight: 700; font-size: 14px; color: @text_strong@; }
QLabel[role="caption"] { font-weight: 600; color: @text_muted@; }
QLabel[role="subtitle"] { font-weight: 600; color: @text_secondary@; }
QLabel[role="note"] { color: @text_muted@; font-size: 11px; }
QLabel[role="small"] { color: @text_muted@; font-size: 10px; border: none; }
QLabel[role="ok"] { color: @ok@; }
QLabel[role="warning"] { color: @warning@; }
QLabel[role="error"] { color: @error@; }
)";

	static QColor Lookup(const palette_t& palette, const char *name) {
		for(const std::pair<const char*, const char*>& kv : palette)
			if(strcmp(kv.first, name) == 0)
				return QColor(kv.second);
		return QColor();
	}

	// Fusion, without icons on dialog buttons ("Close", "OK", ...): the desktop's icon theme may
	// well be dark-on-dark (or light-on-light) here, which made them invisible
	class Style : public QProxyStyle {
	public:
		Style() : QProxyStyle(QStyleFactory::create("Fusion")) {}
		int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr, QStyleHintReturn *return_data = nullptr) const override {
			if(hint == QStyle::SH_DialogButtonBox_ButtonsHaveIcons)
				return 0;
			return QProxyStyle::styleHint(hint, option, widget, return_data);
		}
	};

	Theme Current() {
		return current_theme;
	}

	QString ThemeName(Theme theme) {
		return theme == Theme::LIGHT ? "light" : "dark";
	}

	Theme ThemeFromName(const QString& name) {
		return name == "light" ? Theme::LIGHT : Theme::DARK;
	}

	void SetRole(QWidget *widget, const char *role) {
		if(widget->property("role").toString() == role)
			return;
		widget->setProperty("role", role);
		widget->style()->unpolish(widget);
		widget->style()->polish(widget);
	}

	void MakeTableCopyable(QTableWidget *table) {
		table->setContextMenuPolicy(Qt::CustomContextMenu);
		QObject::connect(table, &QTableWidget::customContextMenuRequested, table, [table](const QPoint& pos) {
			QTableWidgetItem *item = table->itemAt(pos);
			if(!item)
				return;
			int row = item->row();
			if(table->selectionBehavior() == QAbstractItemView::SelectRows && table->selectionMode() != QAbstractItemView::NoSelection)
				table->selectRow(row);
			QString cell = item->text();
			QStringList row_cells;
			for(int column = 0; column < table->columnCount(); column++) {
				QTableWidgetItem *it = table->item(row, column);
				row_cells << (it ? it->text().replace('\n', ", ") : QString());
			}

			QMenu menu(table);
			menu.addAction("Copy", table, [cell]() {QApplication::clipboard()->setText(cell);});
			menu.addAction("Copy row", table, [row_cells]() {QApplication::clipboard()->setText(row_cells.join('\t'));});
			menu.exec(table->viewport()->mapToGlobal(pos));
		});
	}

	void Apply(QApplication& app, Theme theme) {
		current_theme = theme;
		const palette_t& palette = theme == Theme::LIGHT ? light_palette : dark_palette;

		struct {QColor *color; const char *name;} colors[] = {
			{&window_bg, "window_bg"}, {&panel_bg, "panel_bg"}, {&bar_bg, "bar_bg"}, {&input_bg, "input_bg"},
			{&border, "border"}, {&input_border, "input_border"}, {&text, "text"}, {&text_strong, "text_strong"},
			{&text_secondary, "text_secondary"}, {&text_muted, "text_muted"}, {&text_disabled, "text_disabled"},
			{&accent, "accent"}, {&on_accent, "on_accent"}, {&ok, "ok"}, {&warning, "warning"}, {&error, "error"}, {&info, "info"},
			{&meter_border, "meter_border"}, {&meter_tick, "meter_tick"}, {&meter_label, "meter_label"}, {&meter_track, "meter_track"},
			{&meter_diagonal, "meter_diagonal"}, {&meter_peak, "meter_peak"}, {&meter_points, "meter_points"}, {&meter_cap, "meter_cap"},
			{&meter_channel, "meter_channel"},
			{&cu_audio, "cu_audio"}, {&cu_data, "cu_data"}, {&cu_data_hatch, "cu_data_hatch"}, {&cu_outline, "cu_outline"},
			{&timeline_block, "timeline_block"}, {&timeline_border, "timeline_border"}, {&timeline_time, "timeline_time"},
			{&slide_waiting, "slide_waiting"},
		};
		for(auto& c : colors)
			*c.color = Lookup(palette, c.name);

		// Fusion: the same flat rendering on every desktop, which the style sheet then colours
		if(!dynamic_cast<Style*>(app.style()))
			app.setStyle(new Style());

		// the palette covers whatever the style sheet doesn't (standard dialogs, message boxes)
		QPalette p;
		p.setColor(QPalette::Window, window_bg);
		p.setColor(QPalette::WindowText, text);
		p.setColor(QPalette::Base, input_bg);
		p.setColor(QPalette::AlternateBase, panel_bg);
		p.setColor(QPalette::Text, text);
		p.setColor(QPalette::Button, panel_bg);
		p.setColor(QPalette::ButtonText, text);
		p.setColor(QPalette::ToolTipBase, Lookup(palette, "tooltip_bg"));
		p.setColor(QPalette::ToolTipText, text);
		p.setColor(QPalette::Highlight, Lookup(palette, "text_selection_bg"));
		p.setColor(QPalette::HighlightedText, text_strong);
		p.setColor(QPalette::Link, accent);
		p.setColor(QPalette::PlaceholderText, text_disabled);
		p.setColor(QPalette::Disabled, QPalette::WindowText, text_disabled);
		p.setColor(QPalette::Disabled, QPalette::Text, text_disabled);
		p.setColor(QPalette::Disabled, QPalette::ButtonText, text_disabled);
		app.setPalette(p);

		// a style sheet can only take an image (combo box arrow, check mark) from a file: they are
		// drawn here, in the theme's colours, and saved next to the user's other EDIScope files
		QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
		QDir().mkpath(dir);
		auto arrow = [&](const QString& name, const QColor& color) {
			QPixmap pixmap(20, 12);	// drawn at 2x, shown at 10x6
			pixmap.fill(Qt::transparent);
			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setPen(Qt::NoPen);
			painter.setBrush(color);
			painter.drawPolygon(QPolygonF({QPointF(1, 1), QPointF(19, 1), QPointF(10, 11)}));
			painter.end();
			QString path = dir + "/" + name + "-" + ThemeName(theme) + ".png";
			pixmap.save(path);
			return path;
		};
		auto check_mark = [&](const QString& name, const QColor& color) {
			QPixmap pixmap(28, 28);	// drawn at 2x, shown at 14x14
			pixmap.fill(Qt::transparent);
			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setPen(QPen(color, 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			painter.drawPolyline(QPolygonF({QPointF(6, 14.5), QPointF(11.5, 20), QPointF(22, 8)}));
			painter.end();
			QString path = dir + "/" + name + "-" + ThemeName(theme) + ".png";
			pixmap.save(path);
			return path;
		};

		QString sheet = style_sheet;
		sheet.replace("@ARROW_DOWN@", arrow("arrow-down", text_secondary));
		sheet.replace("@ARROW_DOWN_DISABLED@", arrow("arrow-down-disabled", text_disabled));
		sheet.replace("@CHECK_MARK@", check_mark("check-mark", on_accent));
		for(const std::pair<const char*, const char*>& kv : palette)
			sheet.replace(QString("@") + kv.first + "@", kv.second);

		app.setStyleSheet(sheet);

		// custom-drawn widgets read the colours above when they paint
		for(QWidget *widget : QApplication::allWidgets())
			widget->update();
	}

	QFont MonoFont() {
		return QFontDatabase::systemFont(QFontDatabase::FixedFont);
	}
}


// --- EDIScopeQtPanel -----------------------------------------------------------------
EDIScopeQtPanel::EDIScopeQtPanel(const QString& title, QWidget *parent) : QFrame(parent) {
	setObjectName("panel");

	QFrame *head = new QFrame(this);
	head->setObjectName("panelHead");
	QHBoxLayout *head_layout = new QHBoxLayout(head);
	head_layout->setContentsMargins(12, 7, 12, 7);

	title_label = new QLabel(title, head);
	title_label->setObjectName("panelTitle");
	info_label = new QLabel(head);
	info_label->setObjectName("panelInfo");
	info_label->setFont(EDIScopeQtTheme::MonoFont());

	head_layout->addWidget(title_label);
	head_layout->addStretch(1);
	head_layout->addWidget(info_label);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(1, 1, 1, 1);
	layout->setSpacing(0);
	layout->addWidget(head);

	body_layout = new QVBoxLayout();
	body_layout->setContentsMargins(0, 0, 0, 0);
	body_layout->setSpacing(0);
	layout->addLayout(body_layout, 1);
}
