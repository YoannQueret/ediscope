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

#ifndef EDISCOPE_QT_DL_PLUS_H_
#define EDISCOPE_QT_DL_PLUS_H_

#include <map>
#include <string>

#include <QTableWidget>

#include "pad_decoder.h"


// --- EDIScopeQtDLPlusTable -----------------------------------------------------------------
// one row per DL Plus content type, sorted by content type. A running ITEM object is marked with
// "▶" (and highlighted), a deleted object is struck through (and greyed out) - never by colour alone
class EDIScopeQtDLPlusTable : public QTableWidget {
	Q_OBJECT
private:
	struct Object {
		std::string text;
		bool deleted;
	};
	std::map<int, Object> objects;	// content type -> object
	bool item_running;

	void Render();
public:
	EDIScopeQtDLPlusTable(QWidget *parent = nullptr);

	void UpdateDLPlusInfo(const DL_STATE& dl);
	void ClearDLPlusInfo();
};

#endif /* EDISCOPE_QT_DL_PLUS_H_ */
