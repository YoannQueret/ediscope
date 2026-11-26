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

#include "ediscope_gtk_links.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "tools.h"


static const char *LINKS_CSS = R"CSS(
.links-empty {
	color: #9299a5;
	font-size: 13px;
}
.links-title {
	font-weight: 700;
	font-size: 14px;
	color: #20242c;
}
.links-stage-label {
	font-weight: 600;
	font-size: 11px;
	color: #8d94a0;
}
)CSS";


// --- EDIScopeGTKLinksWindow -----------------------------------------------------------------
EDIScopeGTKLinksWindow::EDIScopeGTKLinksWindow() :
		main_box(Gtk::ORIENTATION_VERTICAL),
		linking_page(Gtk::ORIENTATION_VERTICAL, 10),
		frequencies_page(Gtk::ORIENTATION_VERTICAL),
		oe_services_page(Gtk::ORIENTATION_VERTICAL),
		ensemble_eid(-1),
		positioned(false)
{
	set_title("Links");
	set_type_hint(Gdk::WINDOW_TYPE_HINT_UTILITY);
	set_default_size(820, 480);
	// deletable: closing via the window manager's own close button is handled (kept alive, just
	// hidden) by EDIScopeGTK, which also owns the toggle button that must stay in sync with it

	Glib::RefPtr<Gtk::CssProvider> css_provider = Gtk::CssProvider::create();
	css_provider->load_from_data(LINKS_CSS);
	Gtk::StyleContext::add_provider_for_screen(Gdk::Screen::get_default(), css_provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	empty_label.set_text("No audio service known yet on this ensemble.");
	empty_label.get_style_context()->add_class("links-empty");
	empty_label.set_margin_top(24);
	empty_label.set_line_wrap(true);

	list_store = Gtk::ListStore::create(list_cols);
	services_list.set_model(list_store);
	services_list.append_column("Service", list_cols.col_label);
	services_list.append_column("Status", list_cols.col_status);
	services_list.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &EDIScopeGTKLinksWindow::on_services_list_selection_changed));

	list_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
	list_scroll.add(services_list);
	list_scroll.set_size_request(280, -1);

	// --- "Linking" tab (FIG 0/6, per selected service) ---
	linking_placeholder.set_text("Select a service on the left to see its Service linking information (FIG 0/6), if any.");
	linking_placeholder.get_style_context()->add_class("links-empty");
	linking_placeholder.set_halign(Gtk::ALIGN_START);
	linking_placeholder.set_line_wrap(true);

	linking_title.get_style_context()->add_class("links-title");
	linking_title.set_halign(Gtk::ALIGN_START);
	linking_title.set_line_wrap(true);

	linking_grid.set_row_spacing(4);
	linking_grid.set_column_spacing(10);
	{
		Gtk::Label *lsn_label = Gtk::manage(new Gtk::Label("Linkage set:"));
		lsn_label->get_style_context()->add_class("links-stage-label");
		lsn_label->set_halign(Gtk::ALIGN_START);
		linking_grid.attach(*lsn_label, 0, 0, 1, 1);
		linking_lsn_value.set_halign(Gtk::ALIGN_START);
		linking_lsn_value.set_selectable(true);
		linking_grid.attach(linking_lsn_value, 1, 0, 1, 1);

		Gtk::Label *flags_label = Gtk::manage(new Gtk::Label("Properties:"));
		flags_label->get_style_context()->add_class("links-stage-label");
		flags_label->set_halign(Gtk::ALIGN_START);
		linking_grid.attach(*flags_label, 0, 1, 1, 1);
		linking_flags_value.set_halign(Gtk::ALIGN_START);
		linking_grid.attach(linking_flags_value, 1, 1, 1, 1);
	}

	members_store = Gtk::ListStore::create(members_cols);
	members_view.set_model(members_store);
	AppendTopAlignedColumn(members_view, "Type", members_cols.col_type);
	AppendTopAlignedColumn(members_view, "Id", members_cols.col_id);
	AppendTopAlignedColumn(members_view, "Detail", members_cols.col_detail);
	for(int i = 0; i < 3; i++)
		members_view.get_column(i)->set_resizable(true);

	members_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	members_scroll.add(members_view);
	members_scroll.set_vexpand(true);

	linking_page.pack_start(linking_placeholder, false, false);
	linking_page.pack_start(linking_title, false, false);
	linking_page.pack_start(linking_grid, false, false);
	linking_page.pack_start(members_scroll, true, true);
	linking_page.set_margin_start(10);
	linking_page.set_margin_end(10);
	linking_page.set_margin_top(10);
	linking_page.set_margin_bottom(10);

	// --- "All Frequencies" tab (FIG 0/21, every known entry) ---
	frequencies_scope_label.set_text("Every Frequency Information (FIG 0/21) entry known on this ensemble - not limited to the service selected on the left.");
	frequencies_scope_label.get_style_context()->add_class("links-stage-label");
	frequencies_scope_label.set_halign(Gtk::ALIGN_START);
	frequencies_scope_label.set_line_wrap(true);
	frequencies_scope_label.set_margin_bottom(6);

	frequencies_empty_label.set_text("No Frequency Information (FIG 0/21) decoded yet.");
	frequencies_empty_label.get_style_context()->add_class("links-empty");
	frequencies_empty_label.set_margin_top(24);
	frequencies_empty_label.set_line_wrap(true);

	frequencies_store = Gtk::ListStore::create(frequencies_cols);
	frequencies_view.set_model(frequencies_store);
	AppendTopAlignedColumn(frequencies_view, "Id", frequencies_cols.col_id);
	AppendTopAlignedColumn(frequencies_view, "Frequencies", frequencies_cols.col_frequencies);
	AppendTopAlignedColumn(frequencies_view, "Channel", frequencies_cols.col_channel);
	AppendTopAlignedColumn(frequencies_view, "Range", frequencies_cols.col_range);
	AppendTopAlignedColumn(frequencies_view, "Applies to", frequencies_cols.col_scope);
	AppendTopAlignedColumn(frequencies_view, "LSN", frequencies_cols.col_lsn);
	for(int i = 0; i < 6; i++)
		frequencies_view.get_column(i)->set_resizable(true);

	frequencies_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	frequencies_scroll.add(frequencies_view);

	frequencies_page.pack_start(frequencies_scope_label, false, false);
	frequencies_page.pack_start(frequencies_empty_label, false, false);
	frequencies_page.pack_start(frequencies_scroll, true, true);
	frequencies_page.set_margin_start(10);
	frequencies_page.set_margin_end(10);
	frequencies_page.set_margin_top(10);
	frequencies_page.set_margin_bottom(10);

	// --- "OE Services" tab (FIG 0/24, every known entry) ---
	oe_services_scope_label.set_text("Every OE Services (FIG 0/24) entry known on this ensemble - not limited to the service selected on the left.");
	oe_services_scope_label.get_style_context()->add_class("links-stage-label");
	oe_services_scope_label.set_halign(Gtk::ALIGN_START);
	oe_services_scope_label.set_line_wrap(true);
	oe_services_scope_label.set_margin_bottom(6);

	oe_services_empty_label.set_text("No OE Services (FIG 0/24) decoded yet.");
	oe_services_empty_label.get_style_context()->add_class("links-empty");
	oe_services_empty_label.set_margin_top(24);
	oe_services_empty_label.set_line_wrap(true);

	oe_services_store = Gtk::ListStore::create(oe_services_cols);
	oe_services_view.set_model(oe_services_store);
	oe_services_view.append_column("SId", oe_services_cols.col_sid);
	oe_services_view.append_column("Scope", oe_services_cols.col_scope);
	oe_services_view.append_column("CAId", oe_services_cols.col_ca);
	oe_services_view.append_column("Ensemble(s)", oe_services_cols.col_eids);
	for(int i = 0; i < 4; i++)
		oe_services_view.get_column(i)->set_resizable(true);

	oe_services_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	oe_services_scroll.add(oe_services_view);

	oe_services_page.pack_start(oe_services_scope_label, false, false);
	oe_services_page.pack_start(oe_services_empty_label, false, false);
	oe_services_page.pack_start(oe_services_scroll, true, true);
	oe_services_page.set_margin_start(10);
	oe_services_page.set_margin_end(10);
	oe_services_page.set_margin_top(10);
	oe_services_page.set_margin_bottom(10);

	detail_notebook.append_page(linking_page, "Linking");
	detail_notebook.append_page(frequencies_page, "All Frequencies");
	detail_notebook.append_page(oe_services_page, "OE Services");
	detail_notebook.set_vexpand(true);

	paned.pack1(list_scroll, false, false);
	paned.pack2(detail_notebook, true, false);
	paned.set_position(280);

	main_box.pack_start(empty_label, false, false);
	main_box.pack_start(paned, true, true);
	add(main_box);

	show_all_children();
	empty_label.hide();
	linking_title.hide();
	linking_grid.hide();
	members_scroll.hide();
	frequencies_scroll.hide();
	oe_services_scroll.hide();
}

void EDIScopeGTKLinksWindow::AlignToParent() {
	// only once, the first time the window is shown - afterwards it stays wherever the user left
	// it (moved or not), it doesn't stick to / follow the main window around
	if(positioned)
		return;
	positioned = true;

	int x, y, w, h;
	get_transient_for()->get_position(x, y);
	get_transient_for()->get_size(w, h);

	move(x + w + 20, y);
}

void EDIScopeGTKLinksWindow::SetEnsembleEid(int eid) {
	ensemble_eid = eid;
}

void EDIScopeGTKLinksWindow::AppendTopAlignedColumn(Gtk::TreeView& view, const Glib::ustring& title, const Gtk::TreeModelColumn<Glib::ustring>& column) {
	Gtk::CellRendererText *renderer = Gtk::manage(new Gtk::CellRendererText());
	renderer->property_yalign() = 0.0;
	int col_index = view.append_column(title, *renderer) - 1;
	view.get_column(col_index)->add_attribute(renderer->property_text(), column);
}

Glib::ustring EDIScopeGTKLinksWindow::FormatMemberType(const FIC_LINKED_SERVICE& member) {
	switch(member.type) {
	case FIC_LINKED_SERVICE::DAB_SID: return "DAB";
	case FIC_LINKED_SERVICE::RDS_PI: return "FM (RDS)";
	case FIC_LINKED_SERVICE::DRM_AMSS_ID: return "DRM/AMSS";
	case FIC_LINKED_SERVICE::DATA_SID: return "Data";
	}
	return "";
}

Glib::ustring EDIScopeGTKLinksWindow::FormatMemberId(const FIC_LINKED_SERVICE& member) {
	switch(member.type) {
	case FIC_LINKED_SERVICE::DATA_SID:
		return "SId " + StringTools::IntToHex((int) member.id, 8).substr(2);
	case FIC_LINKED_SERVICE::RDS_PI:
		return "PI " + StringTools::IntToHex((int) member.id, 4).substr(2);
	case FIC_LINKED_SERVICE::DAB_SID:
	case FIC_LINKED_SERVICE::DRM_AMSS_ID:
	default:
		return "SId " + StringTools::IntToHex((int) member.id, 4).substr(2);
	}
}

Glib::ustring EDIScopeGTKLinksWindow::FormatMemberDetail(const FIC_LINKED_SERVICE& member) const {
	if(member.type == FIC_LINKED_SERVICE::DAB_SID) {
		std::string detail = member.ecc != FIC_LINKED_SERVICE::ecc_none ? "ECC " + StringTools::IntToHex(member.ecc, 2).substr(2) + ", " : "";

		// a FIG 0/6 DAB_SID member is always a 16-bit SId (P/D=0 set) - FIG 0/24 (OE Services)
		// resolves which ensemble(s) actually carry it, by the same SId
		std::vector<int> eids;
		for(bool oe : {false, true}) {
			std::map<int64_t,FIC_OE_SERVICE>::const_iterator it = oe_services.find(FICDecoder::OEServiceKey(oe, false, member.id));
			if(it != oe_services.end())
				eids.insert(eids.end(), it->second.eids.begin(), it->second.eids.end());
		}

		if(eids.empty())
			return detail + "ensemble unknown (no matching FIG 0/24 entry yet)";

		for(size_t i = 0; i < eids.size(); i++) {
			if(i)
				detail += "\n";
			detail += "EId " + StringTools::IntToHex(eids[i], 4).substr(2);

			for(bool oe : {false, true}) {
				std::map<int,FIC_FREQUENCY_INFO>::const_iterator fit = freq_infos.find(FICDecoder::FrequencyInfoKey(oe, eids[i], FIC_FREQUENCY_INFO::DAB_ENSEMBLE));
				if(fit != freq_infos.end()) {
					detail += " (" + FormatFrequencies(fit->second).raw() + ")";
					break;
				}
			}
		}
		return detail;
	}

	if(member.type == FIC_LINKED_SERVICE::RDS_PI) {
		for(bool oe : {false, true}) {
			std::map<int,FIC_FREQUENCY_INFO>::const_iterator it = freq_infos.find(FICDecoder::FrequencyInfoKey(oe, member.id, FIC_FREQUENCY_INFO::FM_RDS));
			if(it != freq_infos.end())
				return FormatFrequencies(it->second);
		}
		return "(frequency unknown - no matching FIG 0/21 entry yet)";
	}

	if(member.type == FIC_LINKED_SERVICE::DRM_AMSS_ID) {
		// IdLQ "11" doesn't distinguish DRM from AMSS - try both
		for(FIC_FREQUENCY_INFO::RANGE range : {FIC_FREQUENCY_INFO::DRM, FIC_FREQUENCY_INFO::AMSS}) {
			for(bool oe : {false, true}) {
				std::map<int,FIC_FREQUENCY_INFO>::const_iterator it = freq_infos.find(FICDecoder::FrequencyInfoKey(oe, member.id, range));
				if(it != freq_infos.end())
					return FormatFrequencies(it->second);
			}
		}
		return "(frequency unknown - no matching FIG 0/21 entry yet)";
	}

	return "";
}

Glib::ustring EDIScopeGTKLinksWindow::FormatRangeName(FIC_FREQUENCY_INFO::RANGE range) {
	switch(range) {
	case FIC_FREQUENCY_INFO::DAB_ENSEMBLE: return "DAB";
	case FIC_FREQUENCY_INFO::FM_RDS: return "FM";
	case FIC_FREQUENCY_INFO::DRM: return "DRM";
	case FIC_FREQUENCY_INFO::AMSS: return "AMSS";
	}
	return "";
}

Glib::ustring EDIScopeGTKLinksWindow::FormatFrequencies(const FIC_FREQUENCY_INFO& info) {
	std::string result;
	char buf[32];

	switch(info.range) {
	case FIC_FREQUENCY_INFO::DAB_ENSEMBLE:
		for(const FIC_FI_DAB_FREQ& f : info.dab_freqs) {
			if(!result.empty())
				result += ", ";
			snprintf(buf, sizeof(buf), "%.3f MHz", f.freq_khz / 1000.0);
			result += buf;
			if(f.geographically_adjacent)
				result += " (adjacent)";
		}
		break;
	case FIC_FREQUENCY_INFO::FM_RDS:
		for(const FIC_FI_FM_FREQ& f : info.fm_freqs) {
			if(!result.empty())
				result += ", ";
			snprintf(buf, sizeof(buf), "%.1f MHz", f.freq_mhz);
			result += buf;
		}
		break;
	case FIC_FREQUENCY_INFO::DRM:
	case FIC_FREQUENCY_INFO::AMSS:
		for(const FIC_FI_OTHER_FREQ& f : info.other_freqs) {
			if(!result.empty())
				result += ", ";
			snprintf(buf, sizeof(buf), "%.3f MHz", f.freq_khz / 1000.0);
			result += buf;
		}
		break;
	}

	return result.empty() ? "(none)" : result;
}

Glib::ustring EDIScopeGTKLinksWindow::FormatDABChannels(const FIC_FREQUENCY_INFO& info) {
	if(info.range != FIC_FREQUENCY_INFO::DAB_ENSEMBLE)
		return "";

	// standard EUREKA-147 Band III channel raster (ETSI EN 300 401 annex B); 16 kHz steps, so an
	// exact match is expected for any properly encoded FIG 0/21 DAB_ENSEMBLE frequency
	static const struct {const char *channel; double freq_khz;} BAND_III[] = {
		{"5A", 174928}, {"5B", 176640}, {"5C", 178352}, {"5D", 180064},
		{"6A", 181936}, {"6B", 183648}, {"6C", 185360}, {"6D", 187072},
		{"7A", 188928}, {"7B", 190640}, {"7C", 192352}, {"7D", 194064},
		{"8A", 195936}, {"8B", 197648}, {"8C", 199360}, {"8D", 201072},
		{"9A", 202928}, {"9B", 204640}, {"9C", 206352}, {"9D", 208064},
		{"10A", 209936}, {"10B", 211648}, {"10C", 213360}, {"10D", 215072},
		{"11A", 216928}, {"11B", 218640}, {"11C", 220352}, {"11D", 222064},
		{"12A", 223936}, {"12B", 225648}, {"12C", 227360}, {"12D", 229072},
		{"13A", 230784}, {"13B", 232496}, {"13C", 234208}, {"13D", 235776}, {"13E", 237488}, {"13F", 239200},
	};

	std::string result;
	for(const FIC_FI_DAB_FREQ& f : info.dab_freqs) {
		if(!result.empty())
			result += ", ";

		const char *channel = nullptr;
		for(const auto& ch : BAND_III) {
			if(std::abs(f.freq_khz - ch.freq_khz) < 1.0) {
				channel = ch.channel;
				break;
			}
		}
		result += channel ? channel : "?";
	}

	return result;
}

std::vector<int> EDIScopeGTKLinksWindow::FindLSNsForMember(FIC_LINKED_SERVICE::TYPE type, uint32_t id) const {
	std::vector<int> lsns;
	for(const std::pair<const int,FIC_SERVICE_LINKING_SET>& kv : linking_sets) {
		for(const FIC_LINKED_SERVICE& member : kv.second.members) {
			if(member.type == type && member.id == id) {
				if(std::find(lsns.begin(), lsns.end(), kv.second.lsn) == lsns.end())
					lsns.push_back(kv.second.lsn);
				break;
			}
		}
	}
	std::sort(lsns.begin(), lsns.end());
	return lsns;
}

std::vector<int> EDIScopeGTKLinksWindow::FindLSNsForFrequencyInfo(const FIC_FREQUENCY_INFO& info) const {
	if(info.range == FIC_FREQUENCY_INFO::FM_RDS)
		return FindLSNsForMember(FIC_LINKED_SERVICE::RDS_PI, info.id);

	if(info.range == FIC_FREQUENCY_INFO::DRM || info.range == FIC_FREQUENCY_INFO::AMSS)
		// IdLQ "11" doesn't distinguish DRM from AMSS in FIG 0/6 either - a DRM_AMSS_ID member
		// could refer to either range, so match on id alone
		return FindLSNsForMember(FIC_LINKED_SERVICE::DRM_AMSS_ID, info.id);

	if(info.range == FIC_FREQUENCY_INFO::DAB_ENSEMBLE) {
		// this entry is keyed by EId, but a FIG 0/6 DAB_SID member only ever gives a service SId
		// - go through FIG 0/24 (OE Services) to find which SId(s) are carried on this EId, then
		// match those SId(s) against the linking sets' members
		std::vector<int> lsns;
		for(const std::pair<const int64_t,FIC_OE_SERVICE>& kv : oe_services) {
			bool carries_this_eid = std::find(kv.second.eids.begin(), kv.second.eids.end(), (int) info.id) != kv.second.eids.end();
			if(!carries_this_eid)
				continue;
			for(int lsn : FindLSNsForMember(FIC_LINKED_SERVICE::DAB_SID, kv.second.sid))
				if(std::find(lsns.begin(), lsns.end(), lsn) == lsns.end())
					lsns.push_back(lsn);
		}
		std::sort(lsns.begin(), lsns.end());
		return lsns;
	}

	return std::vector<int>();
}

Glib::ustring EDIScopeGTKLinksWindow::FormatLSNList(const std::vector<int>& lsns) {
	std::string result;
	for(int lsn : lsns) {
		if(!result.empty())
			result += "\n";
		result += StringTools::IntToHex(lsn, 3);
	}
	return result;
}

Gtk::TreeModel::iterator EDIScopeGTKLinksWindow::FindRow(int sid, int scids) {
	for(Gtk::TreeModel::iterator it = list_store->children().begin(); it != list_store->children().end(); ++it)
		if((int) (*it)[list_cols.col_sid] == sid && (int) (*it)[list_cols.col_scids] == scids)
			return it;
	return Gtk::TreeModel::iterator();
}

void EDIScopeGTKLinksWindow::RefreshRowStatus(const Gtk::TreeModel::iterator& row_it) {
	Gtk::TreeModel::Row row = *row_it;
	int sid = row[list_cols.col_sid];

	for(const std::pair<const int,FIC_SERVICE_LINKING_SET>& kv : linking_sets) {
		if(kv.second.key_service_sid == sid) {
			row[list_cols.col_status] = "Linked";
			return;
		}
	}
	row[list_cols.col_status] = "";
}

void EDIScopeGTKLinksWindow::ShowDetailFor(int sid, int scids) {
	Gtk::TreeModel::iterator row_it = FindRow(sid, scids);
	if(!row_it) {
		linking_title.hide();
		linking_grid.hide();
		members_scroll.hide();
		linking_placeholder.show();
		return;
	}

	// a service could in principle be the key service of more than one set (e.g. a national and
	// an international one) - only the first found is shown
	const FIC_SERVICE_LINKING_SET *set = nullptr;
	for(const std::pair<const int,FIC_SERVICE_LINKING_SET>& kv : linking_sets) {
		if(kv.second.key_service_sid == sid) {
			set = &kv.second;
			break;
		}
	}

	if(!set) {
		linking_title.hide();
		linking_grid.hide();
		members_scroll.hide();
		linking_placeholder.show();
		return;
	}

	linking_placeholder.hide();
	linking_title.show();
	linking_grid.show();
	members_scroll.show();

	Gtk::TreeModel::Row row = *row_it;
	Glib::ustring label = row[list_cols.col_label];
	linking_title.set_text(label + "  (SId " + StringTools::SidToHex(sid) + ")");

	linking_lsn_value.set_text(StringTools::IntToHex(set->lsn, 3));
	linking_flags_value.set_text(std::string(set->active ? "active" : "inactive") + ", " +
			(set->hard_link ? "hard link (same programme)" : "soft link (related programme)") +
			(set->international ? ", international" : ", national") +
			(set->is_data ? ", data service" : ""));

	members_store->clear();
	for(const FIC_LINKED_SERVICE& member : set->members) {
		Gtk::TreeModel::iterator member_it = members_store->append();
		Gtk::TreeModel::Row member_row = *member_it;
		member_row[members_cols.col_type] = FormatMemberType(member);
		member_row[members_cols.col_id] = FormatMemberId(member);
		member_row[members_cols.col_detail] = FormatMemberDetail(member);
	}
}

void EDIScopeGTKLinksWindow::RebuildFrequenciesTab() {
	frequencies_store->clear();

	frequencies_empty_label.set_visible(freq_infos.empty());
	frequencies_scroll.set_visible(!freq_infos.empty());

	for(const std::pair<const int,FIC_FREQUENCY_INFO>& kv : freq_infos) {
		const FIC_FREQUENCY_INFO& info = kv.second;

		Gtk::TreeModel::iterator row_it = frequencies_store->append();
		Gtk::TreeModel::Row row = *row_it;
		row[frequencies_cols.col_lsn] = FormatLSNList(FindLSNsForFrequencyInfo(info));
		row[frequencies_cols.col_range] = FormatRangeName(info.range);
		bool is_this_tuned_ensemble = !info.other_ensemble && info.range == FIC_FREQUENCY_INFO::DAB_ENSEMBLE && (int) info.id == ensemble_eid;
		row[frequencies_cols.col_scope] = is_this_tuned_ensemble ? "This ensemble (tuned, own transmitters)" :
				info.other_ensemble ? "Other ensemble/service" : "This ensemble/service";

		std::string id_str;
		if(info.range == FIC_FREQUENCY_INFO::DAB_ENSEMBLE)
			id_str = "EId " + StringTools::IntToHex((int) info.id, 4).substr(2);
		else if(info.range == FIC_FREQUENCY_INFO::FM_RDS)
			id_str = "PI " + StringTools::IntToHex((int) info.id, 4).substr(2);
		else
			id_str = "SId " + StringTools::IntToHex((int) info.id, 4).substr(2);
		row[frequencies_cols.col_id] = id_str;

		row[frequencies_cols.col_frequencies] = FormatFrequencies(info);
		row[frequencies_cols.col_channel] = FormatDABChannels(info);
	}
}

void EDIScopeGTKLinksWindow::RebuildOEServicesTab() {
	oe_services_store->clear();

	oe_services_empty_label.set_visible(oe_services.empty());
	oe_services_scroll.set_visible(!oe_services.empty());

	for(const std::pair<const int64_t,FIC_OE_SERVICE>& kv : oe_services) {
		const FIC_OE_SERVICE& service = kv.second;

		Gtk::TreeModel::iterator row_it = oe_services_store->append();
		Gtk::TreeModel::Row row = *row_it;
		row[oe_services_cols.col_sid] = "SId " + StringTools::IntToHex((int) service.sid, service.is_data ? 8 : 4).substr(2);
		row[oe_services_cols.col_scope] = service.not_in_tuned_ensemble ? "Other ensemble" : "This ensemble";
		row[oe_services_cols.col_ca] = service.ca_id != 0 ? StringTools::IntToHex(service.ca_id, 2).substr(2) : "(none)";

		std::string eids_str;
		for(int eid : service.eids) {
			if(!eids_str.empty())
				eids_str += ", ";
			eids_str += "EId " + StringTools::IntToHex(eid, 4).substr(2);
		}
		row[oe_services_cols.col_eids] = eids_str;
	}
}

void EDIScopeGTKLinksWindow::on_services_list_selection_changed() {
	Gtk::TreeModel::iterator it = services_list.get_selection()->get_selected();
	if(!it) {
		ShowDetailFor(-1, -1);
		return;
	}

	ShowDetailFor((*it)[list_cols.col_sid], (*it)[list_cols.col_scids]);
}

void EDIScopeGTKLinksWindow::SetServices(const std::vector<SERVICE_ENTRY>& services) {
	int prev_sid = -1, prev_scids = -1;
	Gtk::TreeModel::iterator prev_it = services_list.get_selection()->get_selected();
	if(prev_it) {
		prev_sid = (*prev_it)[list_cols.col_sid];
		prev_scids = (*prev_it)[list_cols.col_scids];
	}

	list_store->clear();

	Gtk::TreeModel::iterator restore_it;
	for(const SERVICE_ENTRY& s : services) {
		Gtk::TreeModel::iterator row_it = list_store->append();
		Gtk::TreeModel::Row row = *row_it;
		row[list_cols.col_sid] = s.sid;
		row[list_cols.col_scids] = s.scids;
		row[list_cols.col_label] = s.label;
		RefreshRowStatus(row_it);

		if(s.sid == prev_sid && s.scids == prev_scids)
			restore_it = row_it;
	}

	empty_label.set_visible(services.empty());
	list_scroll.set_visible(!services.empty());

	if(restore_it) {
		services_list.get_selection()->select(restore_it);
	} else if(!services.empty()) {
		services_list.get_selection()->select(list_store->children().begin());
	} else {
		ShowDetailFor(-1, -1);
	}
}

void EDIScopeGTKLinksWindow::UpdateServiceLinking(int key, const FIC_SERVICE_LINKING_SET& set) {
	linking_sets[key] = set;

	for(Gtk::TreeModel::iterator it = list_store->children().begin(); it != list_store->children().end(); ++it)
		RefreshRowStatus(it);

	// the LSN column of the "All Frequencies" tab is resolved against linking_sets
	RebuildFrequenciesTab();

	Gtk::TreeModel::iterator selected_it = services_list.get_selection()->get_selected();
	if(selected_it)
		ShowDetailFor((*selected_it)[list_cols.col_sid], (*selected_it)[list_cols.col_scids]);
}

void EDIScopeGTKLinksWindow::UpdateFrequencyInfo(int key, const FIC_FREQUENCY_INFO& info) {
	freq_infos[key] = info;
	RebuildFrequenciesTab();

	// a newly (or freshly) arrived frequency might resolve/refresh an FM/DRM/AMSS member's
	// frequency column for whichever service is currently selected
	Gtk::TreeModel::iterator selected_it = services_list.get_selection()->get_selected();
	if(selected_it)
		ShowDetailFor((*selected_it)[list_cols.col_sid], (*selected_it)[list_cols.col_scids]);
}

void EDIScopeGTKLinksWindow::UpdateOEService(int64_t key, const FIC_OE_SERVICE& service) {
	oe_services[key] = service;
	RebuildOEServicesTab();

	// a newly (or freshly) arrived OE Services entry might also resolve/refresh the LSN column of
	// a DAB ensemble row in the "All Frequencies" tab (indirect EId -> SId -> LSN match)
	RebuildFrequenciesTab();

	// ...and a DAB_SID member's ensemble/frequency columns for whichever service is selected
	Gtk::TreeModel::iterator selected_it = services_list.get_selection()->get_selected();
	if(selected_it)
		ShowDetailFor((*selected_it)[list_cols.col_sid], (*selected_it)[list_cols.col_scids]);
}

void EDIScopeGTKLinksWindow::Clear() {
	list_store->clear();
	linking_sets.clear();
	freq_infos.clear();
	oe_services.clear();
	RebuildFrequenciesTab();
	RebuildOEServicesTab();
	empty_label.show();
	list_scroll.hide();
	ShowDetailFor(-1, -1);
}
