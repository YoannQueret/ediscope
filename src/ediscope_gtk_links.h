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

#ifndef EDISCOPE_GTK_LINKS_H_
#define EDISCOPE_GTK_LINKS_H_

#include <map>
#include <string>
#include <vector>

#include <gtkmm.h>

#include "fic_decoder.h"


// --- EDIScopeGTKLinksWindow -----------------------------------------------------------------
// floating window showing Service linking (ETSI EN 300 401 clause 8.1.15, FIG 0/6), Frequency
// Information (clause 8.1.8, FIG 0/21) and OE Services (clause 8.1.10, FIG 0/24), if the
// ensemble carries any - the signalling behind "service following": alternative ways (other DAB
// ensembles, FM/RDS, DRM, AMSS) to receive the same or a related service, and the actual
// frequencies for those alternatives. A service in a linkage set is only ever identified by SId
// (or RDS PI-code etc.) - a DAB alternative's SId is cross-referenced against FIG 0/24 to find
// which ensemble(s) carry it, and from there against FIG 0/21 to resolve an actual frequency
class EDIScopeGTKLinksWindow : public Gtk::Window {
public:
	// one row of the services list, as handed in by EDIScopeGTK
	struct SERVICE_ENTRY {
		int sid;
		int scids;
		std::string label;
	};

private:
	struct ServicesListColumns : public Gtk::TreeModelColumnRecord {
		Gtk::TreeModelColumn<int> col_sid;
		Gtk::TreeModelColumn<int> col_scids;
		Gtk::TreeModelColumn<Glib::ustring> col_label;
		Gtk::TreeModelColumn<Glib::ustring> col_status;

		ServicesListColumns() {
			add(col_sid); add(col_scids); add(col_label); add(col_status);
		}
	};

	struct MembersColumns : public Gtk::TreeModelColumnRecord {
		Gtk::TreeModelColumn<Glib::ustring> col_type;
		Gtk::TreeModelColumn<Glib::ustring> col_id;
		Gtk::TreeModelColumn<Glib::ustring> col_detail;

		MembersColumns() {
			add(col_type); add(col_id); add(col_detail);
		}
	};

	struct FrequenciesColumns : public Gtk::TreeModelColumnRecord {
		Gtk::TreeModelColumn<Glib::ustring> col_id;
		Gtk::TreeModelColumn<Glib::ustring> col_frequencies;
		Gtk::TreeModelColumn<Glib::ustring> col_channel;
		Gtk::TreeModelColumn<Glib::ustring> col_range;
		Gtk::TreeModelColumn<Glib::ustring> col_scope;
		Gtk::TreeModelColumn<Glib::ustring> col_lsn;

		FrequenciesColumns() {
			add(col_id); add(col_frequencies); add(col_channel); add(col_range); add(col_scope); add(col_lsn);
		}
	};

	struct OEServicesColumns : public Gtk::TreeModelColumnRecord {
		Gtk::TreeModelColumn<Glib::ustring> col_sid;
		Gtk::TreeModelColumn<Glib::ustring> col_scope;
		Gtk::TreeModelColumn<Glib::ustring> col_ca;
		Gtk::TreeModelColumn<Glib::ustring> col_eids;

		OEServicesColumns() {
			add(col_sid); add(col_scope); add(col_ca); add(col_eids);
		}
	};

	Gtk::Box main_box;
	Gtk::Label empty_label;
	Gtk::Paned paned;

	Gtk::ScrolledWindow list_scroll;
	Gtk::TreeView services_list;
	ServicesListColumns list_cols;
	Glib::RefPtr<Gtk::ListStore> list_store;

	Gtk::Notebook detail_notebook;

	// "Linking" tab (per selected service)
	Gtk::Box linking_page;
	Gtk::Label linking_placeholder;
	Gtk::Label linking_title;
	Gtk::Grid linking_grid;
	Gtk::Label linking_lsn_value;
	Gtk::Label linking_flags_value;
	Gtk::ScrolledWindow members_scroll;
	Gtk::TreeView members_view;
	MembersColumns members_cols;
	Glib::RefPtr<Gtk::ListStore> members_store;

	// "All Frequencies" tab (every known FIG 0/21 entry, independent of the selected service)
	Gtk::Box frequencies_page;
	Gtk::Label frequencies_scope_label;
	Gtk::Label frequencies_empty_label;
	Gtk::ScrolledWindow frequencies_scroll;
	Gtk::TreeView frequencies_view;
	FrequenciesColumns frequencies_cols;
	Glib::RefPtr<Gtk::ListStore> frequencies_store;

	// "OE Services" tab (every known FIG 0/24 entry, independent of the selected service)
	Gtk::Box oe_services_page;
	Gtk::Label oe_services_scope_label;
	Gtk::Label oe_services_empty_label;
	Gtk::ScrolledWindow oe_services_scroll;
	Gtk::TreeView oe_services_view;
	OEServicesColumns oe_services_cols;
	Glib::RefPtr<Gtk::ListStore> oe_services_store;

	std::map<int,FIC_SERVICE_LINKING_SET> linking_sets;	// FICDecoder::ServiceLinkingKey() -> set
	std::map<int,FIC_FREQUENCY_INFO> freq_infos;			// FICDecoder::FrequencyInfoKey() -> info
	std::map<int64_t,FIC_OE_SERVICE> oe_services;			// FICDecoder::OEServiceKey() -> service
	int ensemble_eid;
	bool positioned;

	// plain text column whose cell is top-aligned, not vertically centered - so that a row stays
	// readable once one of its columns (e.g. a multi-line LSN or Detail list) grows taller than
	// the others
	static void AppendTopAlignedColumn(Gtk::TreeView& view, const Glib::ustring& title, const Gtk::TreeModelColumn<Glib::ustring>& column);

	static Glib::ustring FormatMemberType(const FIC_LINKED_SERVICE& member);
	static Glib::ustring FormatMemberId(const FIC_LINKED_SERVICE& member);
	Glib::ustring FormatMemberDetail(const FIC_LINKED_SERVICE& member) const;
	static Glib::ustring FormatRangeName(FIC_FREQUENCY_INFO::RANGE range);
	static Glib::ustring FormatFrequencies(const FIC_FREQUENCY_INFO& info);
	// Band III/L-Band channel label(s) (e.g. "9C"), same order/count as FormatFrequencies - empty
	// for a non-DAB_ENSEMBLE entry, or for a frequency outside the standard EUREKA-147 raster
	static Glib::ustring FormatDABChannels(const FIC_FREQUENCY_INFO& info);

	// LSN(s) (linkage set number(s), as shown in the "Linking" tab) of every linking set that
	// lists the given member (matched by type + id) as one of its alternatives
	std::vector<int> FindLSNsForMember(FIC_LINKED_SERVICE::TYPE type, uint32_t id) const;
	// same, but for a FIG 0/21 entry - direct id match for FM/DRM/AMSS, or, for a DAB ensemble
	// entry (keyed by EId, not SId), indirectly via whichever OE Services (FIG 0/24) entries
	// list that EId
	std::vector<int> FindLSNsForFrequencyInfo(const FIC_FREQUENCY_INFO& info) const;
	static Glib::ustring FormatLSNList(const std::vector<int>& lsns);

	Gtk::TreeModel::iterator FindRow(int sid, int scids);
	void RefreshRowStatus(const Gtk::TreeModel::iterator& row_it);
	void ShowDetailFor(int sid, int scids);
	void RebuildFrequenciesTab();
	void RebuildOEServicesTab();
	void on_services_list_selection_changed();
public:
	EDIScopeGTKLinksWindow();

	void AlignToParent();

	void SetEnsembleEid(int eid);

	// replaces the full service list (e.g. on open, or whenever the ensemble's services change)
	void SetServices(const std::vector<SERVICE_ENTRY>& services);

	void UpdateServiceLinking(int key, const FIC_SERVICE_LINKING_SET& set);
	void UpdateFrequencyInfo(int key, const FIC_FREQUENCY_INFO& info);
	void UpdateOEService(int64_t key, const FIC_OE_SERVICE& service);

	void Clear();
};

#endif /* EDISCOPE_GTK_LINKS_H_ */
