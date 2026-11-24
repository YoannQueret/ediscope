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

#ifndef EDISCOPE_GTK_RDNS_H_
#define EDISCOPE_GTK_RDNS_H_

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtkmm.h>

#include "radiodns.h"


// --- EDIScopeGTKRDNSWindow -----------------------------------------------------------------
// floating window listing every audio service in the ensemble, letting the user trigger a
// RadioDNS (ETSI TS 103 270) resolution for one or all of them and see exactly which stage of
// the chain (DNS CNAME -> DNS SRV -> HTTP GET of the SI.xml) it got to - a "traceroute" for
// RadioDNS. EDIScopeGTK is the one that actually owns the background resolution thread(s); this
// window only renders whatever it's handed and asks for a (re)test via a callback.
class EDIScopeGTKRDNSWindow : public Gtk::Window {
public:
	// one row of the services list, as handed in by EDIScopeGTK
	struct SERVICE_ENTRY {
		int sid;
		int scids;
		int ecc;	// effective ECC (this service's own override, or the ensemble's) - -1 if unknown
		int eid;
		std::string label;
	};

	typedef std::function<void(int sid, int scids, int ecc, int eid)> TestRequestedSlot;

private:
	struct ServicesListColumns : public Gtk::TreeModelColumnRecord {
		Gtk::TreeModelColumn<int> col_sid;
		Gtk::TreeModelColumn<int> col_scids;
		Gtk::TreeModelColumn<int> col_ecc;
		Gtk::TreeModelColumn<int> col_eid;
		Gtk::TreeModelColumn<Glib::ustring> col_label;
		Gtk::TreeModelColumn<Glib::ustring> col_status;
		Gtk::TreeModelColumn<Glib::ustring> col_status_color;

		ServicesListColumns() {
			add(col_sid); add(col_scids); add(col_ecc); add(col_eid);
			add(col_label); add(col_status); add(col_status_color);
		}
	};

	Gtk::Box main_box;
	Gtk::Box toolbar;
	Gtk::Button test_all_button;
	Gtk::Label empty_label;

	Gtk::Paned paned;

	Gtk::ScrolledWindow list_scroll;
	Gtk::TreeView services_list;
	ServicesListColumns list_cols;
	Glib::RefPtr<Gtk::ListStore> list_store;

	Gtk::Box detail_box;
	Gtk::Label detail_placeholder;
	Gtk::Box detail_header_box;
	Gtk::Label detail_title;
	Gtk::Button test_selected_button;
	Gtk::Grid detail_grid;
	Gtk::Label detail_fqdn_value;
	Gtk::Label detail_gcc_value;
	Gtk::Label detail_cname_value;
	Gtk::Label detail_srv_value;
	Gtk::Label detail_http_value;

	// the fetched SI.xml, decoded - replaces a plain "SI.xml" frame with tabs, since there's more
	// in there than the raw text: names/descriptions, logos (fetched over plain HTTP, independent
	// of the broadcast carousel) and bearers (every way to actually receive this service)
	Gtk::Notebook result_notebook;

	Gtk::ScrolledWindow info_scroll;
	Gtk::Box info_box;
	Gtk::Label info_placeholder;
	Gtk::Label info_provider_name;
	Gtk::Label info_provider_desc;
	Gtk::FlowBox info_provider_logos_flow;	// the serviceProvider's own logos - shown here, not under "Logos" (which is this service's own)
	Gtk::Label info_service_name;
	Gtk::Label info_service_desc;

	Gtk::ScrolledWindow logos_scroll;
	Gtk::FlowBox logos_flow;
	Gtk::Label logos_empty_label;

	struct BearersColumns : public Gtk::TreeModelColumnRecord {
		Gtk::TreeModelColumn<Glib::ustring> col_id;
		Gtk::TreeModelColumn<Glib::ustring> col_bitrate;
		Gtk::TreeModelColumn<Glib::ustring> col_cost;
		Gtk::TreeModelColumn<Glib::ustring> col_mime;
		Gtk::TreeModelColumn<int> col_bitrate_sort;	// not displayed - numeric sort key for col_bitrate ("128 kBit/s" sorts wrong as text)

		BearersColumns() {
			add(col_id); add(col_bitrate); add(col_cost); add(col_mime); add(col_bitrate_sort);
		}
	};
	Gtk::ScrolledWindow bearers_scroll;
	Gtk::TreeView bearers_view;
	BearersColumns bearers_cols;
	Glib::RefPtr<Gtk::ListStore> bearers_store;

	// right-click menu, shown only over a stream bearer (a plain http(s):// URL - not a "dab:"/
	// "fm:" one, which aren't addresses of anything this app could open or usefully copy as a link)
	Gtk::Menu bearer_context_menu;
	Gtk::MenuItem bearer_open_menu_item;
	Gtk::MenuItem bearer_copy_menu_item;
	std::string context_bearer_url;

	Gtk::ScrolledWindow xml_scroll;
	Gtk::TextView xml_textview;
	Glib::RefPtr<Gtk::TextBuffer> xml_buffer;

	Glib::RefPtr<Gtk::CssProvider> css_provider;

	std::map<std::pair<int,int>, RDNS_RESULT> results;	// (sid,scids) -> last known result
	std::map<std::pair<int,int>, bool> pending;			// (sid,scids) -> test currently in flight
	bool positioned;

	TestRequestedSlot test_requested_slot;

	static Glib::ustring ShortStatus(const RDNS_RESULT& result, Glib::ustring& color);
	static Glib::RefPtr<Gdk::Pixbuf> ScaleToThumbnail(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf);
	static bool OnLogoQueryTooltip(int x, int y, bool keyboard_tooltip, const Glib::RefPtr<Gtk::Tooltip>& tooltip, Glib::RefPtr<Gdk::Pixbuf> full_pixbuf);
	static Gtk::Widget *BuildLogoBox(const RDNS_XML_LOGO& logo);
	static bool IsStreamBearer(const std::string& id);
	void PopulateResultTabs(const RDNS_RESULT& result);
	bool on_bearers_view_button_release_event(GdkEventButton *button_event);
	void on_bearer_open_clicked();
	void on_bearer_copy_clicked();
	Gtk::TreeModel::iterator FindRow(int sid, int scids);
	void RefreshRowStatus(const Gtk::TreeModel::iterator& row_it);
	void RenderStatusCell(Gtk::CellRenderer* renderer, const Gtk::TreeModel::iterator& it);
	int BearersTableColumnCompare(int column_id, const Gtk::TreeModel::iterator& a, const Gtk::TreeModel::iterator& b);
	void ShowDetailFor(int sid, int scids);
	void RequestTest(int sid, int scids, int ecc, int eid);
	void on_services_list_selection_changed();
	void on_test_all_clicked();
	void on_test_selected_clicked();
public:
	EDIScopeGTKRDNSWindow();

	void AlignToParent();

	// replaces the full service list (e.g. on open, or whenever the ensemble's services change) -
	// keeps any already-known result for a (sid,scids) pair still present in the new list
	void SetServices(const std::vector<SERVICE_ENTRY>& services);

	// reports the outcome of a resolution requested via RequestTest()/test_requested_slot
	void UpdateResult(int sid, int scids, const RDNS_RESULT& result);

	void Clear();

	void SetTestRequestedSlot(const TestRequestedSlot& slot) {test_requested_slot = slot;}
};

#endif /* EDISCOPE_GTK_RDNS_H_ */
