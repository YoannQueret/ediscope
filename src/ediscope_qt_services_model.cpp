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

#include "ediscope_qt_services_model.h"
#include "ediscope_qt_theme.h"

#include <set>

#include <QFont>


static const char* column_titles[EDIScopeQtServicesModel::COL_COUNT] = {
	"SubChId", "SID", "ECC", "Label", "Short label", "CUs", "Protection", "Bit rate",
	"Format", "Samplerate", "Audio", "Language", "PTY", "SPI/SI", "SPI/PI", "rDNS", "Slot"
};


// --- EDIScopeQtServicesModel -----------------------------------------------------------------
int EDIScopeQtServicesModel::rowCount(const QModelIndex& parent) const {
	return parent.isValid() ? 0 : (int) services.size();
}

int EDIScopeQtServicesModel::columnCount(const QModelIndex& parent) const {
	return parent.isValid() ? 0 : COL_COUNT;
}

QVariant EDIScopeQtServicesModel::data(const QModelIndex& index, int role) const {
	if(!index.isValid() || index.row() >= (int) services.size())
		return QVariant();

	const LISTED_SERVICE& service = services[index.row()];
	int column = index.column();

	switch(role) {
	case Qt::DisplayRole:
		if(column == COL_SLOT || !text_provider)
			return QVariant();
		return QString::fromStdString(text_provider(service, column));
	case Qt::ToolTipRole:
		if(!tooltip_provider)
			return QVariant();
		{
			std::string text = tooltip_provider(service);
			return text.empty() ? QVariant() : QVariant(QString::fromStdString(text));
		}
	case Qt::TextAlignmentRole:
		if(IsCheckmarkColumn(column))
			return QVariant(Qt::AlignCenter);
		return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
	case Qt::ForegroundRole:
		// not a playable service - de-emphasized (italic too, see FontRole)
		if(!service.is_audio)
			return EDIScopeQtTheme::text_muted;
		if(IsCheckmarkColumn(column))
			return EDIScopeQtTheme::ok;
		return QVariant();
	case Qt::FontRole:
		if(!service.is_audio) {
			QFont font;
			font.setItalic(true);
			return font;
		}
		if(IsCheckmarkColumn(column)) {
			QFont font;
			font.setBold(true);
			return font;
		}
		return QVariant();
	}
	return QVariant();
}

QVariant EDIScopeQtServicesModel::headerData(int section, Qt::Orientation orientation, int role) const {
	if(orientation != Qt::Horizontal || section < 0 || section >= COL_COUNT)
		return QVariant();

	switch(role) {
	case Qt::DisplayRole:
		return QString(column_titles[section]);
	case Qt::ToolTipRole:
		switch(section) {
		case COL_SPI_SI:
			return QString("SPI Service Informations");
		case COL_SPI_PI:
			return QString("SPI Programme Informations");
		case COL_RDNS:
			return QString("RadioDNS");
		}
		return QVariant();
	case Qt::TextAlignmentRole:
		if(IsCheckmarkColumn(section))
			return QVariant(Qt::AlignCenter);
		return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
	}
	return QVariant();
}

Qt::ItemFlags EDIScopeQtServicesModel::flags(const QModelIndex& index) const {
	if(!index.isValid() || index.row() >= (int) services.size())
		return Qt::NoItemFlags;

	// data services (e.g. EPG) are listed for CU accounting only - not selectable/playable
	if(!services[index.row()].is_audio)
		return Qt::ItemIsEnabled;
	return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

int EDIScopeQtServicesModel::Find(int sid, int scids) const {
	for(size_t i = 0; i < services.size(); i++)
		if(services[i].sid == sid && services[i].scids == scids)
			return (int) i;
	return -1;
}

int EDIScopeQtServicesModel::AppendRow(const LISTED_SERVICE& service) {
	int row = (int) services.size();
	beginInsertRows(QModelIndex(), row, row);
	services.push_back(service);
	endInsertRows();
	return row;
}

void EDIScopeQtServicesModel::ChangeRow(int row, const LISTED_SERVICE& service) {
	services[row] = service;
	emit dataChanged(index(row, 0), index(row, COL_COUNT - 1));
}

int EDIScopeQtServicesModel::Upsert(const LISTED_SERVICE& service, bool* was_new) {
	int row = Find(service.sid, service.scids);
	bool add_new_row = row == -1;
	if(add_new_row)
		row = AppendRow(service);
	else
		ChangeRow(row, service);

	if(was_new)
		*was_new = add_new_row;
	return row;
}

void EDIScopeQtServicesModel::Remove(int row) {
	if(row < 0 || row >= (int) services.size())
		return;
	beginRemoveRows(QModelIndex(), row, row);
	services.erase(services.begin() + row);
	endRemoveRows();
}

void EDIScopeQtServicesModel::Clear() {
	beginResetModel();
	services.clear();
	endResetModel();
}

void EDIScopeQtServicesModel::RefreshAll() {
	if(!services.empty())
		emit dataChanged(index(0, 0), index((int) services.size() - 1, COL_COUNT - 1));
}

void EDIScopeQtServicesModel::RebuildDataSubchannelRows(const std::map<int, FIC_SUBCHANNEL>& known_subchannels, const std::map<int, uint32_t>& known_data_service_sids) {
	// sub-channels already represented by a playable (audio) row
	std::set<int> claimed_subchids;
	for(const LISTED_SERVICE& ls : services)
		if(ls.is_audio)
			claimed_subchids.insert(ls.subchid);

	// drop stale data-only rows (sub-channel gone, or now claimed by an audio service)
	for(int row = (int) services.size() - 1; row >= 0; row--) {
		const LISTED_SERVICE& ls = services[row];
		if(!ls.is_audio && (known_subchannels.find(ls.subchid) == known_subchannels.end() || claimed_subchids.find(ls.subchid) != claimed_subchids.end()))
			Remove(row);
	}

	// add/update a non-playable placeholder row for every remaining (data-only) sub-channel,
	// so it's listed - and its CUs accounted for - even though EDIScope can't play it
	for(const std::pair<const int, FIC_SUBCHANNEL>& sc : known_subchannels) {
		if(claimed_subchids.find(sc.first) != claimed_subchids.end() || sc.second.IsNone() || sc.second.pl.empty())
			continue;

		LISTED_SERVICE ls;
		ls.is_audio = false;
		ls.subchid = sc.first;
		ls.pri_comp_subchid = sc.first;
		ls.subchannel = sc.second;
		std::map<int,uint32_t>::const_iterator sid_it = known_data_service_sids.find(sc.first);
		if(sid_it != known_data_service_sids.end())
			ls.sid = (int) sid_it->second;

		int row = -1;
		for(size_t i = 0; i < services.size(); i++)
			if(!services[i].is_audio && services[i].subchid == sc.first) {
				row = (int) i;
				break;
			}

		if(row == -1)
			AppendRow(ls);
		else
			ChangeRow(row, ls);
	}
}

int EDIScopeQtServicesModel::Compare(const LISTED_SERVICE& a, const LISTED_SERVICE& b, int column) const {
	if(column == COL_SLOT || !compare_provider) {
		if(a < b)
			return -1;
		if(b < a)
			return 1;
		return 0;
	}
	return compare_provider(a, b, column);
}


// --- EDIScopeQtServicesSortProxy -----------------------------------------------------------------
bool EDIScopeQtServicesSortProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const {
	const EDIScopeQtServicesModel *model = static_cast<const EDIScopeQtServicesModel*>(sourceModel());
	const LISTED_SERVICE& a = model->At(left.row());
	const LISTED_SERVICE& b = model->At(right.row());

	int result = model->Compare(a, b, left.column());
	// equal values keep the default (slot) order, so the order never depends on insertion order
	if(result == 0)
		result = model->Compare(a, b, EDIScopeQtServicesModel::COL_SLOT);
	return result < 0;
}
