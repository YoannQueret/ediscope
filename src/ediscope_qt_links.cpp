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

#include "ediscope_qt_links.h"
#include "ediscope_qt_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QMenu>
#include <QGridLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

#include "tools.h"


// --- EDIScopeQtLinksWindow -----------------------------------------------------------------
EDIScopeQtLinksWindow::EDIScopeQtLinksWindow(QWidget *parent) :
		QWidget(parent, Qt::Tool),
		ensemble_eid(-1),
		positioned(false) {
	setWindowTitle("Links");
	resize(900, 520);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);

	main_stack = new QStackedWidget(this);
	layout->addWidget(main_stack);

	main_stack->addWidget(CreateEmptyPage("No audio service known yet on this ensemble.", main_stack));

	QSplitter *splitter = new QSplitter(Qt::Horizontal, main_stack);
	splitter->setChildrenCollapsible(false);
	main_stack->addWidget(splitter);

	services_list = CreateTable({"Service", "Status"}, splitter);
	services_list->setSelectionMode(QAbstractItemView::SingleSelection);
	services_list->setSelectionBehavior(QAbstractItemView::SelectRows);
	services_list->setFocusPolicy(Qt::StrongFocus);
	services_list->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	services_list->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	connect(services_list, &QTableWidget::itemSelectionChanged, this, &EDIScopeQtLinksWindow::ShowDetailForSelection);
	splitter->addWidget(services_list);

	QTabWidget *tabs = new QTabWidget(splitter);
	splitter->addWidget(tabs);
	splitter->setSizes({280, 620});
	splitter->setStretchFactor(1, 1);

	// --- "Linking" tab (FIG 0/6, per selected service) ---
	QWidget *linking_page = new QWidget(tabs);
	QVBoxLayout *linking_layout = new QVBoxLayout(linking_page);
	linking_layout->setContentsMargins(10, 10, 10, 10);
	linking_stack = new QStackedWidget(linking_page);
	linking_layout->addWidget(linking_stack);

	linking_stack->addWidget(CreateEmptyPage("Select a service on the left to see its Service linking information (FIG 0/6), if any.", linking_stack));

	QWidget *linking_detail = new QWidget(linking_stack);
	QVBoxLayout *detail_layout = new QVBoxLayout(linking_detail);
	detail_layout->setContentsMargins(0, 0, 0, 0);
	detail_layout->setSpacing(10);

	linking_title = new QLabel(linking_detail);
	linking_title->setStyleSheet("font-weight: 700; font-size: 14px; color: #f3f5f7;");
	linking_title->setWordWrap(true);
	detail_layout->addWidget(linking_title);

	QGridLayout *grid = new QGridLayout();
	grid->setHorizontalSpacing(10);
	grid->setVerticalSpacing(4);
	QLabel *lsn_label = new QLabel("Linkage set:", linking_detail);
	QLabel *flags_label = new QLabel("Properties:", linking_detail);
	for(QLabel *label : {lsn_label, flags_label})
		label->setStyleSheet("font-weight: 600; color: #9aa3ad;");
	linking_lsn_value = new QLabel(linking_detail);
	linking_lsn_value->setFont(EDIScopeQtTheme::MonoFont());
	linking_lsn_value->setTextInteractionFlags(Qt::TextSelectableByMouse);
	linking_flags_value = new QLabel(linking_detail);
	grid->addWidget(lsn_label, 0, 0);
	grid->addWidget(linking_lsn_value, 0, 1);
	grid->addWidget(flags_label, 1, 0);
	grid->addWidget(linking_flags_value, 1, 1);
	grid->setColumnStretch(1, 1);
	detail_layout->addLayout(grid);

	members_view = CreateTable({"Type", "Id", "Detail"}, linking_detail);
	detail_layout->addWidget(members_view, 1);
	linking_stack->addWidget(linking_detail);

	linking_stack->addWidget(CreateEmptyPage("No Service linking information (FIG 0/6) for this service - it isn't the key service of any linkage set.", linking_stack));

	tabs->addTab(linking_page, "Linking");

	// --- "All Frequencies" tab (FIG 0/21, every known entry) ---
	auto create_all_entries_page = [&](const QString& scope, const QString& empty, QStackedWidget*& stack, QTableWidget*& view, const QStringList& headers) {
		QWidget *page = new QWidget(tabs);
		QVBoxLayout *page_layout = new QVBoxLayout(page);
		page_layout->setContentsMargins(10, 10, 10, 10);
		page_layout->setSpacing(6);

		QLabel *scope_label = new QLabel(scope, page);
		scope_label->setWordWrap(true);
		scope_label->setStyleSheet("font-weight: 600; color: #9aa3ad;");
		page_layout->addWidget(scope_label);

		stack = new QStackedWidget(page);
		stack->addWidget(CreateEmptyPage(empty, stack));
		view = CreateTable(headers, stack);
		stack->addWidget(view);
		page_layout->addWidget(stack, 1);
		return page;
	};

	tabs->addTab(create_all_entries_page(
			"Every Frequency Information (FIG 0/21) entry known on this ensemble - not limited to the service selected on the left.",
			"No Frequency Information (FIG 0/21) decoded yet.",
			frequencies_stack, frequencies_view, {"Id", "Frequencies", "Channel", "Range", "Applies to", "LSN"}), "All Frequencies");

	// --- "OE Services" tab (FIG 0/24, every known entry) ---
	tabs->addTab(create_all_entries_page(
			"Every OE Services (FIG 0/24) entry known on this ensemble - not limited to the service selected on the left.",
			"No OE Services (FIG 0/24) decoded yet.",
			oe_services_stack, oe_services_view, {"SId", "Scope", "CAId", "Ensemble(s)"}), "OE Services");

	Clear();
}

QTableWidget* EDIScopeQtLinksWindow::CreateTable(const QStringList& headers, QWidget *parent) {
	QTableWidget *table = new QTableWidget(0, headers.size(), parent);
	table->setHorizontalHeaderLabels(headers);
	table->verticalHeader()->hide();
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	// a row can be selected (and copied), as in the RadioDNS bearers
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setShowGrid(false);
	table->setAlternatingRowColors(true);
	table->setWordWrap(false);
	table->horizontalHeader()->setHighlightSections(false);
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
	table->horizontalHeader()->setStretchLastSection(true);

	EDIScopeQtTheme::MakeTableCopyable(table);
	return table;
}

QWidget* EDIScopeQtLinksWindow::CreateEmptyPage(const QString& text, QWidget *parent) {
	QLabel *label = new QLabel(text, parent);
	label->setWordWrap(true);
	label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	label->setContentsMargins(4, 14, 4, 4);
	label->setStyleSheet("color: #9aa3ad; font-size: 13px;");
	return label;
}

void EDIScopeQtLinksWindow::SetCell(QTableWidget *table, int row, int column, const std::string& text) {
	QTableWidgetItem *item = new QTableWidgetItem(QString::fromStdString(text));
	// top-aligned, so that a row stays readable once one of its cells (e.g. a multi-line LSN or
	// Detail list) grows taller than the others
	item->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
	table->setItem(row, column, item);
}

void EDIScopeQtLinksWindow::closeEvent(QCloseEvent *event) {
	// the application is quitting (the main window is gone): let it close for real
	if(!parentWidget() || !parentWidget()->isVisible()) {
		event->accept();
		return;
	}

	// just hidden, so its content survives; the main window un-toggles its button
	hide();
	event->ignore();
	emit Closed();
}

void EDIScopeQtLinksWindow::AlignToParent() {
	// only once, the first time the window is shown - afterwards it stays wherever the user left
	// it, it doesn't follow the main window around
	if(positioned || !parentWidget())
		return;
	positioned = true;

	QRect parent_frame = parentWidget()->window()->frameGeometry();
	move(parent_frame.right() + 20, parent_frame.top());
}

void EDIScopeQtLinksWindow::SetEnsembleEid(int eid) {
	if(ensemble_eid == eid)
		return;
	ensemble_eid = eid;
	RebuildFrequenciesTab();	// "Applies to" depends on it
}

std::string EDIScopeQtLinksWindow::FormatMemberType(const FIC_LINKED_SERVICE& member) {
	switch(member.type) {
	case FIC_LINKED_SERVICE::DAB_SID: return "DAB";
	case FIC_LINKED_SERVICE::RDS_PI: return "FM (RDS)";
	case FIC_LINKED_SERVICE::DRM_AMSS_ID: return "DRM/AMSS";
	case FIC_LINKED_SERVICE::DATA_SID: return "Data";
	}
	return "";
}

std::string EDIScopeQtLinksWindow::FormatMemberId(const FIC_LINKED_SERVICE& member) {
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

std::string EDIScopeQtLinksWindow::FormatMemberDetail(const FIC_LINKED_SERVICE& member) const {
	if(member.type == FIC_LINKED_SERVICE::DAB_SID) {
		std::string detail = member.ecc != FIC_LINKED_SERVICE::ecc_none ? "ECC " + StringTools::IntToHex(member.ecc, 2).substr(2) + ", " : "";

		// a FIG 0/6 DAB_SID member is always a 16-bit SId - FIG 0/24 (OE Services) resolves which
		// ensemble(s) actually carry it, by the same SId
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
					detail += " (" + FormatFrequencies(fit->second) + ")";
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

std::string EDIScopeQtLinksWindow::FormatRangeName(FIC_FREQUENCY_INFO::RANGE range) {
	switch(range) {
	case FIC_FREQUENCY_INFO::DAB_ENSEMBLE: return "DAB";
	case FIC_FREQUENCY_INFO::FM_RDS: return "FM";
	case FIC_FREQUENCY_INFO::DRM: return "DRM";
	case FIC_FREQUENCY_INFO::AMSS: return "AMSS";
	}
	return "";
}

std::string EDIScopeQtLinksWindow::FormatFrequencies(const FIC_FREQUENCY_INFO& info) {
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

std::string EDIScopeQtLinksWindow::FormatDABChannels(const FIC_FREQUENCY_INFO& info) {
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

std::vector<int> EDIScopeQtLinksWindow::FindLSNsForMember(FIC_LINKED_SERVICE::TYPE type, uint32_t id) const {
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

std::vector<int> EDIScopeQtLinksWindow::FindLSNsForFrequencyInfo(const FIC_FREQUENCY_INFO& info) const {
	if(info.range == FIC_FREQUENCY_INFO::FM_RDS)
		return FindLSNsForMember(FIC_LINKED_SERVICE::RDS_PI, info.id);

	if(info.range == FIC_FREQUENCY_INFO::DRM || info.range == FIC_FREQUENCY_INFO::AMSS)
		// IdLQ "11" doesn't distinguish DRM from AMSS in FIG 0/6 either - match on id alone
		return FindLSNsForMember(FIC_LINKED_SERVICE::DRM_AMSS_ID, info.id);

	if(info.range == FIC_FREQUENCY_INFO::DAB_ENSEMBLE) {
		// keyed by EId, but a FIG 0/6 DAB_SID member only ever gives a service SId - go through
		// FIG 0/24 (OE Services) to find which SId(s) are carried on this EId
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

std::string EDIScopeQtLinksWindow::FormatLSNList(const std::vector<int>& lsns) {
	std::string result;
	for(int lsn : lsns) {
		if(!result.empty())
			result += "\n";
		result += StringTools::IntToHex(lsn, 3);
	}
	return result;
}

const FIC_SERVICE_LINKING_SET* EDIScopeQtLinksWindow::FindKeyServiceSet(int sid) const {
	// a service could in principle be the key service of more than one set (e.g. a national and
	// an international one) - only the first found is shown
	for(const std::pair<const int,FIC_SERVICE_LINKING_SET>& kv : linking_sets)
		if(kv.second.key_service_sid == sid)
			return &kv.second;
	return nullptr;
}

int EDIScopeQtLinksWindow::SelectedRow() const {
	QList<QTableWidgetItem*> items = services_list->selectedItems();
	return items.isEmpty() ? -1 : items.first()->row();
}

void EDIScopeQtLinksWindow::RefreshStatuses() {
	for(int row = 0; row < services_list->rowCount() && row < (int) services.size(); row++)
		SetCell(services_list, row, 1, FindKeyServiceSet(services[row].sid) ? "Linked" : "");
}

void EDIScopeQtLinksWindow::ShowDetailForSelection() {
	int row = SelectedRow();
	bool selected = row >= 0 && row < (int) services.size();
	const FIC_SERVICE_LINKING_SET *set = selected ? FindKeyServiceSet(services[row].sid) : nullptr;
	if(!set) {
		linking_stack->setCurrentIndex(selected ? 2 : 0);	// "no linking for this service" / "select a service"
		return;
	}
	linking_stack->setCurrentIndex(1);

	const SERVICE_ENTRY& service = services[row];
	linking_title->setText(QString::fromStdString(service.label + "  (SId " + StringTools::SidToHex(service.sid) + ")"));

	linking_lsn_value->setText(QString::fromStdString(StringTools::IntToHex(set->lsn, 3)));
	linking_flags_value->setText(QString::fromStdString(std::string(set->active ? "active" : "inactive") + ", " +
			(set->hard_link ? "hard link (same programme)" : "soft link (related programme)") +
			(set->international ? ", international" : ", national") +
			(set->is_data ? ", data service" : "")));

	members_view->setRowCount((int) set->members.size());
	for(size_t i = 0; i < set->members.size(); i++) {
		const FIC_LINKED_SERVICE& member = set->members[i];
		SetCell(members_view, (int) i, 0, FormatMemberType(member));
		SetCell(members_view, (int) i, 1, FormatMemberId(member));
		SetCell(members_view, (int) i, 2, FormatMemberDetail(member));
	}
	members_view->resizeColumnsToContents();
	members_view->resizeRowsToContents();
}

void EDIScopeQtLinksWindow::RebuildFrequenciesTab() {
	frequencies_stack->setCurrentIndex(freq_infos.empty() ? 0 : 1);
	frequencies_view->setRowCount((int) freq_infos.size());

	int row = 0;
	for(const std::pair<const int,FIC_FREQUENCY_INFO>& kv : freq_infos) {
		const FIC_FREQUENCY_INFO& info = kv.second;

		std::string id_str;
		if(info.range == FIC_FREQUENCY_INFO::DAB_ENSEMBLE)
			id_str = "EId " + StringTools::IntToHex((int) info.id, 4).substr(2);
		else if(info.range == FIC_FREQUENCY_INFO::FM_RDS)
			id_str = "PI " + StringTools::IntToHex((int) info.id, 4).substr(2);
		else
			id_str = "SId " + StringTools::IntToHex((int) info.id, 4).substr(2);

		bool is_this_tuned_ensemble = !info.other_ensemble && info.range == FIC_FREQUENCY_INFO::DAB_ENSEMBLE && (int) info.id == ensemble_eid;

		SetCell(frequencies_view, row, 0, id_str);
		SetCell(frequencies_view, row, 1, FormatFrequencies(info));
		SetCell(frequencies_view, row, 2, FormatDABChannels(info));
		SetCell(frequencies_view, row, 3, FormatRangeName(info.range));
		SetCell(frequencies_view, row, 4, is_this_tuned_ensemble ? "This ensemble (tuned, own transmitters)" :
				info.other_ensemble ? "Other ensemble/service" : "This ensemble/service");
		SetCell(frequencies_view, row, 5, FormatLSNList(FindLSNsForFrequencyInfo(info)));
		row++;
	}
	frequencies_view->resizeColumnsToContents();
	frequencies_view->resizeRowsToContents();
}

void EDIScopeQtLinksWindow::RebuildOEServicesTab() {
	oe_services_stack->setCurrentIndex(oe_services.empty() ? 0 : 1);
	oe_services_view->setRowCount((int) oe_services.size());

	int row = 0;
	for(const std::pair<const int64_t,FIC_OE_SERVICE>& kv : oe_services) {
		const FIC_OE_SERVICE& service = kv.second;

		std::string eids_str;
		for(int eid : service.eids) {
			if(!eids_str.empty())
				eids_str += ", ";
			eids_str += "EId " + StringTools::IntToHex(eid, 4).substr(2);
		}

		SetCell(oe_services_view, row, 0, "SId " + StringTools::IntToHex((int) service.sid, service.is_data ? 8 : 4).substr(2));
		SetCell(oe_services_view, row, 1, service.not_in_tuned_ensemble ? "Other ensemble" : "This ensemble");
		SetCell(oe_services_view, row, 2, service.ca_id != 0 ? StringTools::IntToHex(service.ca_id, 2).substr(2) : "(none)");
		SetCell(oe_services_view, row, 3, eids_str);
		row++;
	}
	oe_services_view->resizeColumnsToContents();
}

void EDIScopeQtLinksWindow::SetServices(const std::vector<SERVICE_ENTRY>& new_services) {
	// keep the selection on the same service, if it's still there
	int prev_sid = -1, prev_scids = -1;
	int prev_row = SelectedRow();
	if(prev_row >= 0 && prev_row < (int) services.size()) {
		prev_sid = services[prev_row].sid;
		prev_scids = services[prev_row].scids;
	}

	services = new_services;

	services_list->blockSignals(true);
	services_list->clearSelection();
	services_list->setRowCount((int) services.size());
	int restore_row = -1;
	for(size_t i = 0; i < services.size(); i++) {
		SetCell(services_list, (int) i, 0, services[i].label);
		if(services[i].sid == prev_sid && services[i].scids == prev_scids)
			restore_row = (int) i;
	}
	RefreshStatuses();

	main_stack->setCurrentIndex(services.empty() ? 0 : 1);

	if(restore_row == -1 && !services.empty())
		restore_row = 0;
	if(restore_row != -1)
		services_list->selectRow(restore_row);
	services_list->blockSignals(false);

	ShowDetailForSelection();
}

void EDIScopeQtLinksWindow::UpdateServiceLinking(int key, const FIC_SERVICE_LINKING_SET& set) {
	linking_sets[key] = set;
	RefreshStatuses();

	// the LSN column of the "All Frequencies" tab is resolved against linking_sets
	RebuildFrequenciesTab();
	ShowDetailForSelection();
}

void EDIScopeQtLinksWindow::UpdateFrequencyInfo(int key, const FIC_FREQUENCY_INFO& info) {
	freq_infos[key] = info;
	RebuildFrequenciesTab();

	// may resolve/refresh a member's frequency for the selected service
	ShowDetailForSelection();
}

void EDIScopeQtLinksWindow::UpdateOEService(int64_t key, const FIC_OE_SERVICE& service) {
	oe_services[key] = service;
	RebuildOEServicesTab();

	// may resolve/refresh the LSN column of a DAB ensemble row in the "All Frequencies" tab
	// (indirect EId -> SId -> LSN match), and a DAB_SID member's ensemble/frequency
	RebuildFrequenciesTab();
	ShowDetailForSelection();
}

void EDIScopeQtLinksWindow::Clear() {
	services.clear();
	services_list->setRowCount(0);
	linking_sets.clear();
	freq_infos.clear();
	oe_services.clear();
	ensemble_eid = -1;
	RebuildFrequenciesTab();
	RebuildOEServicesTab();
	main_stack->setCurrentIndex(0);
	ShowDetailForSelection();
}
