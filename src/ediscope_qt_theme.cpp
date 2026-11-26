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
	const QColor window_bg("#15171b");
	const QColor panel_bg("#1b1f25");
	const QColor bar_bg("#121417");
	const QColor input_bg("#12151a");
	const QColor border("#262b32");
	const QColor input_border("#2b313a");
	const QColor text("#e8ebee");
	const QColor text_strong("#f3f5f7");
	const QColor text_secondary("#c7ccd2");
	const QColor text_muted("#9aa3ad");
	const QColor text_disabled("#5c646e");
	const QColor accent("#5eead4");
	const QColor on_accent("#0d1210");
	const QColor ok("#34d399");
	const QColor warning("#fbbf24");
	const QColor error("#f87171");
	const QColor info("#60a5fa");

	static const char *style_sheet = R"(
QMainWindow, QDialog { background: #15171b; }
QWidget { color: #e8ebee; }

QMenuBar { background: #121417; color: #c7ccd2; border-bottom: 1px solid #262b32; padding: 2px 6px; }
QMenuBar::item { padding: 4px 10px; background: transparent; }
QMenuBar::item:selected { background: #1f242b; border-radius: 4px; }
QMenu { background: #1b1f25; color: #e8ebee; border: 1px solid #2b313a; padding: 4px; }
QMenu::item { padding: 5px 22px 5px 22px; border-radius: 4px; }
QMenu::item:selected { background: #2a3038; }
QMenu::item:disabled { color: #5c646e; }
QMenu::separator { height: 1px; background: #262b32; margin: 4px 6px; }

QToolBar { background: #191c22; border: none; border-bottom: 1px solid #262b32; padding: 6px 10px; spacing: 8px; }
QToolBar::separator { background: #2b313a; width: 1px; margin: 2px 6px; }

QStatusBar { background: #121417; color: #c7ccd2; border-top: 1px solid #262b32; }
QStatusBar::item { border: none; }
QStatusBar QLabel { padding: 0 8px; }

QFrame#panel { background: #1b1f25; border: 1px solid #262b32; border-radius: 10px; }
QFrame#panelHead { background: transparent; border: none; border-bottom: 1px solid #262b32; }
QLabel#panelTitle { color: #f3f5f7; font-weight: 600; }
QLabel#panelInfo { color: #9aa3ad; }

QPushButton, QToolButton {
	background: #12151a; border: 1px solid #2b313a; border-radius: 7px; color: #e8ebee;
	padding: 5px 12px; min-height: 18px;
}
QPushButton:hover, QToolButton:hover { border-color: #3d4550; }
QPushButton:checked, QToolButton:checked { border-color: #5eead4; color: #5eead4; }
QPushButton:disabled, QToolButton:disabled { color: #5c646e; border-color: #22272e; }
QPushButton#connectButton { background: #5eead4; border-color: #5eead4; color: #0d1210; font-weight: 600; padding: 5px 16px; }
QPushButton#connectButton:checked { background: #12151a; color: #5eead4; }
QPushButton#recordButton[recording="true"] { border-color: #f87171; color: #f87171; font-weight: 600; }

QComboBox, QLineEdit {
	background: #12151a; border: 1px solid #2b313a; border-radius: 7px; color: #dfe3e8;
	padding: 4px 8px; selection-background-color: #2e5f59;
}
QComboBox:disabled, QLineEdit:disabled { color: #7b838d; }
QComboBox::drop-down { border: none; border-left: 1px solid #2b313a; width: 26px; }
QComboBox::down-arrow { image: url(@ARROW_DOWN@); width: 10px; height: 6px; }
QComboBox::down-arrow:disabled { image: url(@ARROW_DOWN_DISABLED@); }
QComboBox QAbstractItemView { background: #1b1f25; border: 1px solid #2b313a; selection-background-color: #2a3038; }

QTableView {
	background: #1b1f25; alternate-background-color: #181c21; border: none; gridline-color: #21252b;
	selection-background-color: #23403c; selection-color: #ffffff; outline: 0;
}
QHeaderView::section {
	background: #15181d; color: #aeb6c0; border: none; border-bottom: 1px solid #262b32;
	border-right: 1px solid #1f2329; padding: 5px 8px;
}
QTableCornerButton::section { background: #15181d; border: none; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle { background: #3a4049; border-radius: 5px; min-height: 24px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QTabWidget::pane { border: 1px solid #262b32; border-radius: 8px; background: #1b1f25; top: -1px; }
QTabBar::tab {
	background: #12151a; color: #c7ccd2; border: 1px solid #2b313a; border-bottom: none;
	padding: 6px 14px; margin-right: 2px; border-top-left-radius: 6px; border-top-right-radius: 6px;
}
QTabBar::tab:selected { background: #1b1f25; color: #f3f5f7; border-color: #5eead4; font-weight: 600; }
QTabBar::tab:hover:!selected { border-color: #3d4550; }
QSplitter::handle { background: #262b32; }
QSplitter::handle:horizontal { width: 1px; margin: 0 4px; }

QToolTip { background: #22272e; color: #e8ebee; border: 1px solid #3d4550; padding: 4px; }

QProgressBar { background: #12151a; border: 1px solid #2b313a; border-radius: 4px; text-align: center; color: #c7ccd2; max-height: 14px; }
QProgressBar::chunk { background: #2e8f80; border-radius: 3px; }

QSlider::groove:horizontal { height: 4px; background: #2b313a; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #5eead4; border-radius: 2px; }
QSlider::handle:horizontal { background: #e8ebee; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }
QSlider::handle:horizontal:disabled { background: #5c646e; }
)";

	// Fusion, without icons on dialog buttons ("Close", "OK", ...): the desktop's icon theme may
	// well be dark-on-dark here, which made them invisible
	class Style : public QProxyStyle {
	public:
		Style() : QProxyStyle(QStyleFactory::create("Fusion")) {}
		int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr, QStyleHintReturn *return_data = nullptr) const override {
			if(hint == QStyle::SH_DialogButtonBox_ButtonsHaveIcons)
				return 0;
			return QProxyStyle::styleHint(hint, option, widget, return_data);
		}
	};

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

	void Apply(QApplication& app) {
		// Fusion: the same flat rendering on every desktop, which the style sheet then colours
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
		p.setColor(QPalette::ToolTipBase, QColor("#22272e"));
		p.setColor(QPalette::ToolTipText, text);
		p.setColor(QPalette::Highlight, QColor("#2e5f59"));
		p.setColor(QPalette::HighlightedText, QColor("#ffffff"));
		p.setColor(QPalette::Link, accent);
		p.setColor(QPalette::PlaceholderText, text_disabled);
		p.setColor(QPalette::Disabled, QPalette::WindowText, text_disabled);
		p.setColor(QPalette::Disabled, QPalette::Text, text_disabled);
		p.setColor(QPalette::Disabled, QPalette::ButtonText, text_disabled);
		app.setPalette(p);

		// the combo box arrow: a style sheet can only take it from an image file, so it is drawn
		// here once and saved next to the user's other EDIScope files
		QString sheet = style_sheet;
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
			QString path = dir + "/" + name + ".png";
			pixmap.save(path);
			return path;
		};
		sheet.replace("@ARROW_DOWN@", arrow("arrow-down", text_secondary));
		sheet.replace("@ARROW_DOWN_DISABLED@", arrow("arrow-down-disabled", text_disabled));

		app.setStyleSheet(sheet);
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
