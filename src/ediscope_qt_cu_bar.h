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

#ifndef EDISCOPE_QT_CU_BAR_H_
#define EDISCOPE_QT_CU_BAR_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <QWidget>

#include "fic_decoder.h"

class EDIScopeQtImagePopup;


// --- EDIScopeQtCUBar -----------------------------------------------------------------
// full width bar showing the CIF's 864 Capacity Units, with one block per occupied sub-channel -
// including sub-channels with no (audio) service EDIScope can play (e.g. data/EPG services), so
// the bar's background accurately reflects which CUs are actually free. The selected service's
// block has a thick outline and data blocks are hatched, so none of the states rely on colour alone
class EDIScopeQtCUBar : public QWidget {
	Q_OBJECT
private:
	struct Entry {
		int subchid;
		int sid;	// 16-bit (programme) or 32-bit (data) SId
		bool has_sid;	// sid is only meaningful if true (always true when is_audio)
		int scids;
		size_t start;
		size_t size;
		bool is_audio;
		std::string label;	// service name, as in the services table's Label column
	};

	std::vector<LISTED_SERVICE> services;
	std::map<int, FIC_SUBCHANNEL> subchannels;
	std::vector<Entry> entries;
	int selected_subchid;
	std::function<std::string(const LISTED_SERVICE&)> label_provider;

	// the same popup as the slides': shown at once on hover, following the pointer
	EDIScopeQtImagePopup *popup;
	int popup_subchid;	// the block the popup currently describes, -1 if none

	void RebuildEntries();
	const Entry* GetEntryAt(double x) const;
	static QString EntryText(const Entry& entry);
	void UpdatePopup(const QPoint& pos);
protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void leaveEvent(QEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	bool event(QEvent *event) override;
public:
	static const size_t total_cus = 864;

	EDIScopeQtCUBar(QWidget *parent = nullptr);

	void SetServices(const std::vector<LISTED_SERVICE>& services);
	void SetSubchannels(const std::map<int, FIC_SUBCHANNEL>& subchannels);
	void SetSelectedSubchId(int subchid);
	// the name shown for a service in the tooltip (the same text as the services table's Label column)
	void SetLabelProvider(std::function<std::string(const LISTED_SERVICE&)> provider) {label_provider = provider;}
	size_t UsedCUs() const;
signals:
	// emitted with (sid, scids) when the user clicks a service's block
	void ServiceClicked(int sid, int scids);
};

#endif /* EDISCOPE_QT_CU_BAR_H_ */
