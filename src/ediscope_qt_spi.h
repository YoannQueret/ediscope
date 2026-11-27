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

#ifndef EDISCOPE_QT_SPI_H_
#define EDISCOPE_QT_SPI_H_

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QWidget>

#include "spi_decoder.h"

class EDIScopeQtImagePopup;


// --- EDIScopeQtSPITimeline -----------------------------------------------------------------
// one service's programmes of one day, as blocks on a 24 hour vertical timeline
class EDIScopeQtSPITimeline : public QWidget {
	Q_OBJECT
public:
	struct Block {
		int start_min;		// local time, minutes since midnight
		int duration_min;	// -1 if unknown
		std::string name;
	};
private:
	std::vector<Block> blocks;
protected:
	void paintEvent(QPaintEvent *event) override;
public:
	static constexpr double px_per_min = 1.5;

	EDIScopeQtSPITimeline(QWidget *parent = nullptr);
	void SetBlocks(const std::vector<Block>& blocks);
};


// --- EDIScopeQtSPIWindow -----------------------------------------------------------------
// floating window for the SPI (ETSI TS 102 371/102 818) information decoded from the ensemble's
// data sub-channel, if any, as tabs mirroring SPI's three document types - "Services" (SI: a grid
// of cards, 4 per row, one per service: name, DAB bearer and logos), "Programme" (PI: a per-service
// day timeline) and "Groups" (GI: a grid of cards, one per programme group) - plus "XML" (any of
// the three, reconstructed as readable XML, picked via a dropdown)
class EDIScopeQtSPIWindow : public QWidget {
	Q_OBJECT
private:
	// the main window: not the Qt parent, so that this window can go below it, and be
	// maximised/minimised on its own
	QWidget *main_window;
	static const int cards_per_row = 4;
	static const int logos_per_row = 3;	// leaves each logo room for its caption

	QStackedWidget *services_stack;	// empty message, or the cards
	QLabel *services_message;
	QGridLayout *services_grid;
	QComboBox *programme_selector;
	QStackedWidget *programme_stack;
	EDIScopeQtSPITimeline *programme_timeline;
	QStackedWidget *groups_stack;
	QGridLayout *groups_grid;
	QComboBox *xml_selector;
	QPlainTextEdit *xml_view;

	EDIScopeQtImagePopup *popup;

	// last decoded data of each SPI document type, kept so the selectors can be (re)populated
	// independently of which document last triggered an Update*() call
	SPI_SERVICE_INFORMATION last_si;
	std::map<std::pair<uint32_t,std::string>,SPI_SCHEDULE> last_schedules;	// (service SId, "YYYY-MM-DD")
	SPI_GROUP_INFORMATION last_gi;

	// SId -> best-known FIC label, a fallback when SPI's own SI doesn't describe that service
	std::map<uint32_t,std::string> known_service_names;

	bool positioned;

	static std::string PickName(const std::map<std::string,std::string>& names);
	static std::string BestName(const std::map<std::string,std::string>& long_names, const std::map<std::string,std::string>& medium_names, const std::map<std::string,std::string>& short_names);
	static std::string FormatDABBearer(bool has_dab_bearer, int ecc, int eid, uint32_t sid, bool sid_32bit, int scids);
	static std::string FormatLogoCaption(const SPI_LOGO& logo, size_t data_size);
	std::string FindServiceName(uint32_t sid) const;

	static void ClearGrid(QGridLayout *grid);
	QWidget* BuildLogos(const std::vector<SPI_LOGO>& logos, const std::map<std::string,std::vector<uint8_t>>& logo_data, QWidget *parent);
	QWidget* BuildCard(const std::string& name, const std::string& subtitle, bool subtitle_mono, const std::vector<SPI_LOGO>& logos,
			const std::map<std::string,std::vector<uint8_t>>& logo_data);

	void RebuildProgrammeSelector();
	void RedrawProgrammeTimeline();
	void RedrawXML();
protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
public:
	EDIScopeQtSPIWindow(QWidget *parent);

	// places the window centred on the main window - only the first time it is shown
	void AlignToParent();

	void Update(const SPI_SERVICE_INFORMATION& info, const std::map<std::string,std::vector<uint8_t>>& logo_data);
	void UpdateSchedule(const SPI_SCHEDULE& schedule);
	void UpdateGroupInformation(const SPI_GROUP_INFORMATION& info, const std::map<std::string,std::vector<uint8_t>>& logo_data);
	void SetKnownServiceNames(const std::map<uint32_t,std::string>& names);
	// the SPI data sub-channel, or -1 if the ensemble carries none - so the window can tell "no SPI
	// here" from "SPI found, its Service Information has just not come round in the carousel yet"
	void SetSPIStatus(int subchid);
	void Clear();
signals:
	// the user closed the window (it is only hidden)
	void Closed();
};

#endif /* EDISCOPE_QT_SPI_H_ */
