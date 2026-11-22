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

#include "ediscope_gtk_logs.h"

#include <time.h>


// --- EDIScopeGTKLogsWindow -----------------------------------------------------------------
EDIScopeGTKLogsWindow::EDIScopeGTKLogsWindow() {
	positioned = false;

	set_title("Logs");
	set_type_hint(Gdk::WINDOW_TYPE_HINT_UTILITY);
	set_default_size(600, 300);
	// deletable: closing via the window manager's own close button is handled (kept alive, just
	// hidden) by EDIScopeGTK, which also owns the toggle button that must stay in sync with it

	logs_liststore = Gtk::ListStore::create(logs_cols);
	logs_table.set_model(logs_liststore);
	logs_table.set_headers_visible(false);
	logs_table.append_column("Log", logs_cols.col_message);

	logs_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	logs_scroll.add(logs_table);

	add(logs_scroll);

	show_all_children();
}

void EDIScopeGTKLogsWindow::AlignToParent() {
	// only once, the first time the window is shown - afterwards it stays wherever the user left
	// it (moved or not), it doesn't stick to / follow the main window around
	if(positioned)
		return;
	positioned = true;

	int x, y, w, h;
	get_transient_for()->get_position(x, y);
	get_transient_for()->get_size(w, h);

	// TODO: fix multi head issue
	move(x + w + 20, y);	// add some horizontal padding for WM decoration
}

void EDIScopeGTKLogsWindow::AppendLogMessage(const std::string& message) {
	static const size_t max_log_entries = 1000;

	time_t now = time(nullptr);
	struct tm* now_tm = now != (time_t) -1 ? localtime(&now) : nullptr;
	char ts[10] = "";
	if(now_tm)
		strftime(ts, sizeof(ts), "%H:%M:%S", now_tm);

	// newest entry on top
	Gtk::TreeModel::iterator row_it = logs_liststore->prepend();
	Gtk::TreeModel::Row row = *row_it;
	row[logs_cols.col_message] = std::string(ts) + "  " + message;

	// cap the log size, dropping the oldest entries (now at the end of the list)
	Gtk::TreeModel::Children children = logs_liststore->children();
	while(children.size() > max_log_entries) {
		Gtk::TreeModel::iterator last_it = children.end();
		--last_it;
		logs_liststore->erase(last_it);
	}

	logs_table.scroll_to_row(logs_liststore->get_path(row_it));
}
