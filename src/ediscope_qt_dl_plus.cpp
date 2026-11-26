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

#include "ediscope_qt_dl_plus.h"
#include "ediscope_qt_theme.h"

#include <QHeaderView>


// --- EDIScopeQtDLPlusTable -----------------------------------------------------------------
EDIScopeQtDLPlusTable::EDIScopeQtDLPlusTable(QWidget *parent) : QTableWidget(0, 2, parent), item_running(false) {
	setHorizontalHeaderLabels({"Content type", "Text"});
	horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	horizontalHeader()->setStretchLastSection(true);
	horizontalHeader()->setHighlightSections(false);
	verticalHeader()->hide();
	verticalHeader()->setDefaultSectionSize(24);
	setEditTriggers(QAbstractItemView::NoEditTriggers);
	setSelectionMode(QAbstractItemView::NoSelection);
	setFocusPolicy(Qt::NoFocus);
	setShowGrid(false);
	setWordWrap(false);

	// always 2 rows' worth of height, even with 0 or 1 actual rows; a 3rd+ object scrolls
	int height = horizontalHeader()->sizeHint().height() + 2 * verticalHeader()->defaultSectionSize() + 4;
	setMinimumHeight(height);
	setMaximumHeight(height);
}

void EDIScopeQtDLPlusTable::UpdateDLPlusInfo(const DL_STATE& dl) {
	for(const DL_PLUS_OBJECT& obj : dl.dl_plus_objects) {
		// ignore DUMMY tags
		if(obj.content_type == 0)
			continue;

		bool deleted = obj.text == " ";

		std::map<int, Object>::iterator it = objects.find(obj.content_type);
		if(it == objects.end()) {
			// continue, if no existing object for received deletion
			if(deleted)
				continue;
			it = objects.emplace(obj.content_type, Object{obj.text, false}).first;
		}
		if(!deleted)
			it->second.text = obj.text;
		it->second.deleted = deleted;
	}

	item_running = dl.dl_plus_ir;

	setToolTip(QString("Item toggle bit: %1\nItem running bit: %2").arg(dl.dl_plus_it ? 1 : 0).arg(dl.dl_plus_ir ? 1 : 0));
	Render();
}

void EDIScopeQtDLPlusTable::ClearDLPlusInfo() {
	objects.clear();
	item_running = false;
	setToolTip(QString());
	Render();
}

void EDIScopeQtDLPlusTable::Render() {
	setRowCount((int) objects.size());

	QFont mono = EDIScopeQtTheme::MonoFont();

	int row = 0;
	for(const std::pair<const int, Object>& kv : objects) {
		int content_type = kv.first;
		const Object& obj = kv.second;

		// highlight running ITEM objects - with a marker, not just a colour
		bool running_item = content_type < 12 && item_running;
		QTableWidgetItem *type_item = new QTableWidgetItem(
				QString(running_item ? "▶ " : "   ") + QString::fromStdString(DynamicLabelDecoder::ConvertDLPlusContentTypeToString(content_type)));
		type_item->setFont(mono);
		type_item->setForeground(running_item ? EDIScopeQtTheme::ok : EDIScopeQtTheme::text_secondary);
		if(running_item)
			type_item->setToolTip("ITEM object, item running");
		setItem(row, 0, type_item);

		// color highlighting
		QColor color = EDIScopeQtTheme::text;
		if(content_type == 1)	// ITEM.TITLE
			color = EDIScopeQtTheme::info;
		if(content_type >= 12 && content_type < 31)	// INFO
			color = EDIScopeQtTheme::ok;
		if(content_type >= 41 && content_type < 54)	// INTERACTIVITY
			color = EDIScopeQtTheme::info;

		QTableWidgetItem *text_item = new QTableWidgetItem(QString::fromStdString(obj.text));
		QFont text_font;
		text_font.setBold(content_type < 12);	// highlight ITEM objects
		text_font.setStrikeOut(obj.deleted);	// indicate deleted objects
		text_item->setFont(text_font);
		text_item->setForeground(obj.deleted ? EDIScopeQtTheme::text_disabled : color);
		if(obj.deleted)
			text_item->setToolTip("Deleted object");
		setItem(row, 1, text_item);

		row++;
	}
}
