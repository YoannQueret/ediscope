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

#ifndef EDISCOPE_QT_SERVICES_MODEL_H_
#define EDISCOPE_QT_SERVICES_MODEL_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <QAbstractTableModel>
#include <QSortFilterProxyModel>

#include "fic_decoder.h"


// --- EDIScopeQtServicesModel -----------------------------------------------------------------
// one row per listed (audio) service component, plus one non-playable row per data-only
// sub-channel (e.g. EPG), listed purely so its CUs are accounted for. The text of a cell depends
// on state kept by the main window (current audio format, SPI, RadioDNS, ...), so it is asked
// for through the providers below
class EDIScopeQtServicesModel : public QAbstractTableModel {
	Q_OBJECT
public:
	enum Column {
		COL_SUBCHID, COL_SID, COL_ECC, COL_LABEL, COL_SHORT_LABEL, COL_CUS, COL_PROTECTION, COL_BITRATE,
		COL_FORMAT, COL_SAMPLERATE, COL_AUDIO_MODE, COL_LANGUAGE, COL_PTY, COL_SPI_SI, COL_SPI_PI, COL_RDNS,
		COL_SLOT,	// hidden: the default (slot) order of the services
		COL_COUNT
	};
	static bool IsCheckmarkColumn(int column) {return column == COL_SPI_SI || column == COL_SPI_PI || column == COL_RDNS;}

	typedef std::function<std::string(const LISTED_SERVICE&, int)> TextProvider;
	typedef std::function<std::string(const LISTED_SERVICE&)> ToolTipProvider;
	typedef std::function<int(const LISTED_SERVICE&, const LISTED_SERVICE&, int)> CompareProvider;
private:
	std::vector<LISTED_SERVICE> services;
	TextProvider text_provider;
	ToolTipProvider tooltip_provider;
	CompareProvider compare_provider;

	int AppendRow(const LISTED_SERVICE& service);
	void ChangeRow(int row, const LISTED_SERVICE& service);
public:
	EDIScopeQtServicesModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}

	void SetTextProvider(TextProvider provider) {text_provider = provider;}
	void SetToolTipProvider(ToolTipProvider provider) {tooltip_provider = provider;}
	void SetCompareProvider(CompareProvider provider) {compare_provider = provider;}

	int rowCount(const QModelIndex& parent = QModelIndex()) const override;
	int columnCount(const QModelIndex& parent = QModelIndex()) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
	Qt::ItemFlags flags(const QModelIndex& index) const override;

	const std::vector<LISTED_SERVICE>& Services() const {return services;}
	const LISTED_SERVICE& At(int row) const {return services[row];}
	int Find(int sid, int scids) const;

	// adds or updates the row of this service (matched by SId/SCIdS), returning its row
	int Upsert(const LISTED_SERVICE& service, bool* was_new = nullptr);
	void Remove(int row);
	void Clear();
	// recomputes every cell (after state the providers depend on has changed)
	void RefreshAll();
	// adds/updates/removes the non-playable rows of the data-only sub-channels
	void RebuildDataSubchannelRows(const std::map<int, FIC_SUBCHANNEL>& known_subchannels, const std::map<int, uint32_t>& known_data_service_sids);

	int Compare(const LISTED_SERVICE& a, const LISTED_SERVICE& b, int column) const;
};


// --- EDIScopeQtServicesSortProxy -----------------------------------------------------------------
class EDIScopeQtServicesSortProxy : public QSortFilterProxyModel {
	Q_OBJECT
protected:
	bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
public:
	EDIScopeQtServicesSortProxy(QObject *parent = nullptr) : QSortFilterProxyModel(parent) {}
};

#endif /* EDISCOPE_QT_SERVICES_MODEL_H_ */
