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

#ifndef EDISCOPE_QT_RDNS_H_
#define EDISCOPE_QT_RDNS_H_

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QWidget>

#include "radiodns.h"

class EDIScopeQtImagePopup;


// --- EDIScopeQtRDNSWindow -----------------------------------------------------------------
// floating window listing every audio service in the ensemble, letting the user trigger a
// RadioDNS (ETSI TS 103 270) resolution for one or all of them and see exactly which stage of the
// chain (DNS CNAME -> DNS SRV -> HTTP GET of the SI.xml) it got to - a "traceroute" for RadioDNS.
// The main window owns the background resolution thread(s); this window only renders whatever it
// is handed, and asks for a (re)test through TestRequested
class EDIScopeQtRDNSWindow : public QWidget {
	Q_OBJECT
public:
	// one row of the services list, as handed in by the main window
	struct SERVICE_ENTRY {
		int sid;
		int scids;
		int ecc;	// effective ECC (this service's own override, or the ensemble's) - -1 if unknown
		int eid;
		std::string label;
	};
private:
	// the main window: not the Qt parent, so that this window can go below it, and be
	// maximised/minimised on its own
	QWidget *main_window;
	QStackedWidget *main_stack;	// "no service yet" message, or the list + detail
	QTableWidget *services_list;
	std::vector<SERVICE_ENTRY> services;

	QLabel *detail_title;
	QPushButton *test_selected_button;
	QLabel *stage_values[5];	// FQDN, GCC, DNS CNAME, DNS SRV, HTTP
	QTabWidget *result_tabs;

	// "Info" tab
	QLabel *info_placeholder;
	QLabel *info_provider_name;
	QLabel *info_provider_desc;
	QListWidget *info_provider_logos;	// the serviceProvider's own logos - not under "Logos" (which is this service's own)
	QLabel *info_service_name;
	QLabel *info_service_desc;

	// "Logos", "Bearers" and "SI.xml" tabs
	QStackedWidget *logos_stack;
	QListWidget *logos_list;
	QTableWidget *bearers_view;
	QPlainTextEdit *xml_view;

	EDIScopeQtImagePopup *popup;
	QListWidgetItem *popup_item;	// the logo the popup currently shows

	std::map<std::pair<int,int>, RDNS_RESULT> results;	// (sid,scids) -> last known result
	std::map<std::pair<int,int>, bool> pending;			// (sid,scids) -> test currently in flight
	bool positioned;

	static QListWidget* CreateLogoList(QWidget *parent);
	void FillLogoList(QListWidget *list, const std::vector<RDNS_XML_LOGO>& logos);
	static bool IsStreamBearer(const std::string& id);
	void PopulateResultTabs(const RDNS_RESULT& result);
	int SelectedRow() const;
	void RefreshRowStatus(int row);
	void ShowDetailForSelection();
	void RequestTest(const SERVICE_ENTRY& service);
protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
public:
	EDIScopeQtRDNSWindow(QWidget *parent);

	// places the window right of the main window - only the first time it is shown
	void AlignToParent();

	// replaces the full service list (e.g. on open, or whenever the ensemble's services change) -
	// keeps any already-known result for a (sid,scids) pair still present in the new list
	void SetServices(const std::vector<SERVICE_ENTRY>& services);

	// reports the outcome of a resolution (requested here, or started automatically by the main window)
	void UpdateResult(int sid, int scids, const RDNS_RESULT& result);

	void Clear();
	// re-colours what was coloured when filled (statuses), after a theme change
	void ApplyTheme();
signals:
	void TestRequested(int sid, int scids, int ecc, int eid);
	// the user closed the window (it is only hidden)
	void Closed();
};

#endif /* EDISCOPE_QT_RDNS_H_ */
