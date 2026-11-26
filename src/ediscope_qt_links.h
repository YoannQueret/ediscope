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

#ifndef EDISCOPE_QT_LINKS_H_
#define EDISCOPE_QT_LINKS_H_

#include <map>
#include <string>
#include <vector>

#include <QLabel>
#include <QStackedWidget>
#include <QTableWidget>
#include <QWidget>

#include "fic_decoder.h"


// --- EDIScopeQtLinksWindow -----------------------------------------------------------------
// floating window showing Service linking (ETSI EN 300 401 clause 8.1.15, FIG 0/6), Frequency
// Information (clause 8.1.8, FIG 0/21) and OE Services (clause 8.1.10, FIG 0/24), if the
// ensemble carries any - the signalling behind "service following": alternative ways (other DAB
// ensembles, FM/RDS, DRM, AMSS) to receive the same or a related service, and the actual
// frequencies for those alternatives. A service in a linkage set is only ever identified by SId
// (or RDS PI-code etc.) - a DAB alternative's SId is cross-referenced against FIG 0/24 to find
// which ensemble(s) carry it, and from there against FIG 0/21 to resolve an actual frequency
class EDIScopeQtLinksWindow : public QWidget {
	Q_OBJECT
public:
	// one row of the services list, as handed in by the main window
	struct SERVICE_ENTRY {
		int sid;
		int scids;
		std::string label;
	};
private:
	QStackedWidget *main_stack;	// "no service yet" message, or the list + tabs
	QTableWidget *services_list;
	std::vector<SERVICE_ENTRY> services;

	// "Linking" tab (per selected service)
	QStackedWidget *linking_stack;	// placeholder, or the selected service's linkage set
	QLabel *linking_title;
	QLabel *linking_lsn_value;
	QLabel *linking_flags_value;
	QTableWidget *members_view;

	// "All Frequencies" and "OE Services" tabs (every known entry, independent of the selected service)
	QStackedWidget *frequencies_stack;
	QTableWidget *frequencies_view;
	QStackedWidget *oe_services_stack;
	QTableWidget *oe_services_view;

	std::map<int,FIC_SERVICE_LINKING_SET> linking_sets;	// FICDecoder::ServiceLinkingKey() -> set
	std::map<int,FIC_FREQUENCY_INFO> freq_infos;			// FICDecoder::FrequencyInfoKey() -> info
	std::map<int64_t,FIC_OE_SERVICE> oe_services;			// FICDecoder::OEServiceKey() -> service
	int ensemble_eid;
	bool positioned;

	static QTableWidget* CreateTable(const QStringList& headers, QWidget *parent);
	static QWidget* CreateEmptyPage(const QString& text, QWidget *parent);
	static void SetCell(QTableWidget *table, int row, int column, const std::string& text);

	static std::string FormatMemberType(const FIC_LINKED_SERVICE& member);
	static std::string FormatMemberId(const FIC_LINKED_SERVICE& member);
	std::string FormatMemberDetail(const FIC_LINKED_SERVICE& member) const;
	static std::string FormatRangeName(FIC_FREQUENCY_INFO::RANGE range);
	static std::string FormatFrequencies(const FIC_FREQUENCY_INFO& info);
	// Band III channel label(s) (e.g. "9C"), same order/count as FormatFrequencies - empty for a
	// non-DAB_ENSEMBLE entry; "?" for a frequency outside the standard EUREKA-147 raster
	static std::string FormatDABChannels(const FIC_FREQUENCY_INFO& info);

	// LSN(s) of every linking set that lists the given member (matched by type + id)
	std::vector<int> FindLSNsForMember(FIC_LINKED_SERVICE::TYPE type, uint32_t id) const;
	// same, for a FIG 0/21 entry - direct id match for FM/DRM/AMSS, or, for a DAB ensemble entry
	// (keyed by EId, not SId), indirectly via whichever OE Services (FIG 0/24) entries list that EId
	std::vector<int> FindLSNsForFrequencyInfo(const FIC_FREQUENCY_INFO& info) const;
	static std::string FormatLSNList(const std::vector<int>& lsns);

	const FIC_SERVICE_LINKING_SET* FindKeyServiceSet(int sid) const;
	int SelectedRow() const;
	void RefreshStatuses();
	void ShowDetailForSelection();
	void RebuildFrequenciesTab();
	void RebuildOEServicesTab();
protected:
	void closeEvent(QCloseEvent *event) override;
public:
	EDIScopeQtLinksWindow(QWidget *parent);

	// places the window right of the main window - only the first time it is shown
	void AlignToParent();

	void SetEnsembleEid(int eid);
	// replaces the full service list (e.g. on open, or whenever the ensemble's services change)
	void SetServices(const std::vector<SERVICE_ENTRY>& services);

	void UpdateServiceLinking(int key, const FIC_SERVICE_LINKING_SET& set);
	void UpdateFrequencyInfo(int key, const FIC_FREQUENCY_INFO& info);
	void UpdateOEService(int64_t key, const FIC_OE_SERVICE& service);

	void Clear();
signals:
	// the user closed the window (it is only hidden)
	void Closed();
};

#endif /* EDISCOPE_QT_LINKS_H_ */
