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

#ifndef EDISCOPE_QT_FIG_H_
#define EDISCOPE_QT_FIG_H_

#include <functional>

#include <QLabel>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QWidget>

#include "fig_analyzer.h"


// --- EDIScopeQtFIGHistogram -----------------------------------------------------------------
// the distribution of the intervals between repetitions, one bar per class (relative to the
// nominal interval), with its count and share; each class is marked ✓ (at the nominal rate),
// ~ (slower, but not below one third of it) or ✗ (below) - not by colour alone
class EDIScopeQtFIGHistogram : public QWidget {
	Q_OBJECT
private:
	FIGHistogram histogram;
	double nominal_ms;
	QString title;
protected:
	void paintEvent(QPaintEvent *event) override;
public:
	EDIScopeQtFIGHistogram(QWidget *parent = nullptr);
	void SetHistogram(const FIGHistogram& histogram, double nominal_ms, const QString& title);
	void Clear();
};


// --- EDIScopeQtFIGWindow -----------------------------------------------------------------
// floating window showing the FIG analysis (see FIGAnalyzer): every repetition requirement of
// ETSI TS 103 176 with its verdict (and, for the selected one, the measurement per entity or per
// event), an overview of every FIG seen, and the bursts (CEI, announcements). The analysis runs all
// the time, the window only shows a snapshot of it, refreshed every second while visible
class EDIScopeQtFIGWindow : public QWidget {
	Q_OBJECT
public:
	typedef std::function<FIG_ANALYSIS_SNAPSHOT()> SnapshotProvider;
private:
	SnapshotProvider snapshot_provider;
	FIG_ANALYSIS_SNAPSHOT snapshot;
	QTimer refresh_timer;
	bool positioned;

	QLabel *summary_label;
	QLabel *capacity_label;
	QTableWidget *checks_view;
	QLabel *detail_title;
	QStackedWidget *detail_stack;	// entities (periodic check), or events (burst check)
	QTableWidget *entities_view;
	QTableWidget *check_events_view;
	EDIScopeQtFIGHistogram *histogram_view;
	QString selected_entity;		// name of the entity (or key of the event) selected in the detail
	QTableWidget *figs_view;
	QTableWidget *events_view;
	QString selected_check;			// fig + usage of the selected check, kept across refreshes

	void Refresh();
	void RefreshDetail();
	void RefreshHistogram();
	static QString EventKey(const FIG_ANALYSIS_EVENT& e);
	static void SetVerdictCell(QTableWidget *table, int row, int column, FIGVerdict verdict);
	static void FillEvents(QTableWidget *table, const std::vector<FIG_ANALYSIS_EVENT>& events);
protected:
	void closeEvent(QCloseEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
public:
	EDIScopeQtFIGWindow(QWidget *parent, SnapshotProvider snapshot_provider);

	// places the window right of the main window - only the first time it is shown
	void AlignToParent();

	// re-colours what was coloured when filled (verdicts, summary), after a theme change
	void ApplyTheme() {if(isVisible()) Refresh();}

	static QString VerdictText(FIGVerdict verdict);
	static QColor VerdictColor(FIGVerdict verdict);
signals:
	void ResetRequested();
	// the user closed the window (it is only hidden)
	void Closed();
};

#endif /* EDISCOPE_QT_FIG_H_ */
