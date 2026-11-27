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

#ifndef EDISCOPE_QT_ANNOUNCEMENTS_H_
#define EDISCOPE_QT_ANNOUNCEMENTS_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include <QDateTime>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

#include "fic_decoder.h"


// --- EDIScopeQtAnnouncementsWindow -----------------------------------------------------------------
// floating window analysing the announcements (ETSI EN 300 401 V2.1.1 clause 8.1.6): the clusters
// and their current state (FIG 0/19, 0/26), which service may be interrupted by what (FIG 0/18,
// 0/25), the history of every announcement and the consistency checks of the signalling. It is
// fed all the time (not only while visible), so the history and the checks cover the whole
// connection; the repetition rates themselves are measured by the FIG analysis
class EDIScopeQtAnnouncementsWindow : public QWidget {
	Q_OBJECT
public:
	struct SERVICE_ENTRY {
		int sid;
		bool primary;		// primary component: the one the service's FIG 0/18 support is shown for
		int subchid;
		std::string label;
		uint16_t asu_flags;
		cids_t cids;
	};

	enum class Severity {INFO, WARNING, ERROR};
private:
	struct EVENT {
		QDateTime time;
		QString fig;
		QString cluster;
		QString event;
		QString types;
		QString where;		// SubChId, or target SId/SCIdS
		QString carried_by;
		qint64 duration_ms;	// -1 if not applicable
		QString note;
	};

	struct FINDING {
		Severity severity;
		QString fig;
		QString entity;
		QString text;
		QDateTime first_seen;
		QDateTime last_seen;
		bool present;
	};

	// state
	FIC_ENSEMBLE ensemble;
	bool ensemble_known;
	std::vector<SERVICE_ENTRY> services;
	std::set<int> subchids;		// FIG 0/1
	fic_oe_announcement_supports_t oe_supports;
	fic_oe_asw_clusters_t oe_clusters;
	std::map<uint8_t, QDateTime> since;				// start of the current announcement of a cluster
	std::map<uint32_t, QDateTime> oe_since;			// same, OE cluster (key: FICDecoder::OEAnnouncementKey)
	std::vector<EVENT> history;
	unsigned long history_serial;		// bumped on every change of the history
	unsigned long history_shown_serial;	// the one the table shows
	std::map<QString, FINDING> findings;	// key: FIG + entity + text

	QTimer tick_timer;
	bool positioned;

	// widgets
	QLabel *summary_label;
	QTableWidget *clusters_view;
	QTableWidget *oe_clusters_view;
	QTableWidget *support_view;
	QTableWidget *oe_support_view;
	QTableWidget *history_view;
	QTableWidget *checks_view;
	QTabWidget *tabs;

	void AddEvent(const QString& fig, const QString& cluster, const QString& event, uint16_t asw_flags,
			const QString& where, const QString& carried_by, qint64 duration_ms, const QString& note);
	void Evaluate();
	void Refresh();
	void RefreshClusters();
	void RefreshSupport();
	void RefreshHistory();
	void RefreshChecks();
	void ExportHistory();

	QString SubchannelService(int subchid) const;
	QString ServiceName(int sid) const;
	std::vector<const SERVICE_ENTRY*> ClusterMembers(uint8_t cid) const;
	static QString ClusterName(uint8_t cid);
protected:
	void closeEvent(QCloseEvent *event) override;
	void showEvent(QShowEvent *event) override;
public:
	EDIScopeQtAnnouncementsWindow(QWidget *parent);

	void SetEnsemble(const FIC_ENSEMBLE& ensemble);
	void SetServices(const std::vector<SERVICE_ENTRY>& services);
	void SetSubchannels(const std::set<int>& subchids);
	void UpdateOESupport(const FIC_OE_ANNOUNCEMENT_SUPPORT& support);
	void UpdateOESwitching(uint32_t key, const FIC_OE_ASW_CLUSTER& cluster);
	void Clear();

	// places the window right of the main window - only the first time it is shown
	void AlignToParent();

	// re-colours what was coloured when filled (states, severities), after a theme change
	void ApplyTheme() {history_shown_serial = 0; if(isVisible()) Refresh();}

	// e.g. "Road Traffic flash + News flash"
	static QString TypesString(uint16_t flags);
	static QString SeverityText(Severity severity);
	static QColor SeverityColor(Severity severity);
signals:
	void FIGAnalysisRequested();
	// the user closed the window (it is only hidden)
	void Closed();
};

#endif /* EDISCOPE_QT_ANNOUNCEMENTS_H_ */
