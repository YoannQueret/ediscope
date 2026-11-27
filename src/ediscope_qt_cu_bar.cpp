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

#include "ediscope_qt_cu_bar.h"
#include "ediscope_qt_theme.h"
#include "ediscope_qt_sls.h"

#include <set>

#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>




// --- EDIScopeQtCUBar -----------------------------------------------------------------
EDIScopeQtCUBar::EDIScopeQtCUBar(QWidget *parent) : QWidget(parent), selected_subchid(-1), popup_subchid(-1) {
	setMinimumHeight(44);
	setMaximumHeight(44);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setMouseTracking(true);	// the popup appears on hover and follows the pointer

	popup = new EDIScopeQtImagePopup(this);
	popup->hide();
}

const EDIScopeQtCUBar::Entry* EDIScopeQtCUBar::GetEntryAt(double x) const {
	const double w = width();

	for(const Entry& entry : entries) {
		double x0 = w * entry.start / total_cus;
		double x1 = w * (entry.start + entry.size) / total_cus;
		if(x1 > w)
			x1 = w;

		if(x >= x0 && x < x1)
			return &entry;
	}
	return nullptr;
}

QString EDIScopeQtCUBar::EntryText(const Entry& entry) {
	std::string text;
	if(!entry.label.empty())
		text += "Service: " + entry.label + "\n";
	text += "SubChId: " + std::to_string(entry.subchid);
	if(entry.has_sid)
		text += "\n" "SID: " + StringTools::SidToHex(entry.sid);
	text += "\n" "CUs: " + std::to_string(entry.size) + " (" + std::to_string(entry.start) + "-" + std::to_string(entry.start + entry.size) + ")";
	if(!entry.is_audio)
		text += "\n" "Data service (not playable)";
	return QString::fromStdString(text);
}

void EDIScopeQtCUBar::UpdatePopup(const QPoint& pos) {
	const Entry* entry = GetEntryAt(pos.x());
	if(!entry) {
		popup->hide();
		popup_subchid = -1;
		return;
	}
	if(entry->subchid != popup_subchid || !popup->isVisible()) {
		popup->ShowAt(QCursor::pos(), QImage(), EntryText(*entry));
		popup_subchid = entry->subchid;
	} else {
		popup->FollowCursor(QCursor::pos());
	}
}

void EDIScopeQtCUBar::mouseMoveEvent(QMouseEvent *event) {
	UpdatePopup(event->position().toPoint());
	QWidget::mouseMoveEvent(event);
}

void EDIScopeQtCUBar::leaveEvent(QEvent *event) {
	popup->hide();
	popup_subchid = -1;
	QWidget::leaveEvent(event);
}

void EDIScopeQtCUBar::hideEvent(QHideEvent *event) {
	popup->hide();
	popup_subchid = -1;
	QWidget::hideEvent(event);
}

bool EDIScopeQtCUBar::event(QEvent *event) {
	if(event->type() == QEvent::ToolTip)
		return true;	// the popup replaces the regular tooltip
	return QWidget::event(event);
}

void EDIScopeQtCUBar::SetServices(const std::vector<LISTED_SERVICE>& services) {
	this->services = services;
	RebuildEntries();
}

void EDIScopeQtCUBar::SetSubchannels(const std::map<int, FIC_SUBCHANNEL>& subchannels) {
	this->subchannels = subchannels;
	RebuildEntries();
}

void EDIScopeQtCUBar::RebuildEntries() {
	entries.clear();

	// one block per distinct sub-channel: a service's secondary components share the
	// same sub-channel (and thus the same CUs) as its primary component. Data-only rows
	// (is_audio false) aren't turned into blocks here - they're already covered generically
	// by the subchannels loop below - but their SId (if known) is collected for it to use.
	std::set<int> handled_subchids;
	std::map<int,int> data_sids;
	std::map<int,const LISTED_SERVICE*> data_rows;
	for(const LISTED_SERVICE& service : services) {
		if(!service.is_audio) {
			if(service.sid != LISTED_SERVICE::sid_none)
				data_sids[service.subchid] = service.sid;
			data_rows[service.subchid] = &service;
			continue;
		}
		if(service.subchannel.IsNone() || service.subchannel.pl.empty())
			continue;

		int subchid = service.subchid;
		if(!handled_subchids.insert(subchid).second)
			continue;

		Entry entry;
		entry.subchid = subchid;
		entry.sid = service.sid;
		entry.has_sid = true;
		entry.scids = service.scids;
		entry.start = service.subchannel.start;
		entry.size = service.subchannel.size;
		entry.is_audio = true;
		entry.label = label_provider ? label_provider(service) : FICDecoder::ConvertLabelToUTF8(service.label, nullptr);
		entries.push_back(entry);
	}

	// any remaining sub-channel (most commonly a data service, e.g. EPG) still occupies
	// real CU capacity even though EDIScope can't play it - shown as a distinct block
	// (with its SId, if known), so the bar's background doesn't misleadingly look free there
	for(const std::pair<const int, FIC_SUBCHANNEL>& sc : subchannels) {
		if(sc.second.IsNone() || sc.second.pl.empty() || handled_subchids.find(sc.first) != handled_subchids.end())
			continue;

		std::map<int,int>::const_iterator sid_it = data_sids.find(sc.first);

		Entry entry;
		entry.subchid = sc.first;
		entry.has_sid = sid_it != data_sids.end();
		entry.sid = entry.has_sid ? sid_it->second : 0;
		entry.scids = LISTED_SERVICE::scids_none;
		entry.start = sc.second.start;
		entry.size = sc.second.size;
		entry.is_audio = false;
		std::map<int,const LISTED_SERVICE*>::const_iterator row_it = data_rows.find(sc.first);
		entry.label = row_it != data_rows.end() && label_provider ? label_provider(*row_it->second) : "(data service)";
		entries.push_back(entry);
	}

	update();

	// the block under the pointer may have changed (e.g. a hot reconfiguration)
	if(popup->isVisible()) {
		popup_subchid = -1;	// its text, even for the same block
		UpdatePopup(mapFromGlobal(QCursor::pos()));
	}
}

void EDIScopeQtCUBar::SetSelectedSubchId(int subchid) {
	if(selected_subchid == subchid)
		return;

	selected_subchid = subchid;
	update();
}

size_t EDIScopeQtCUBar::UsedCUs() const {
	size_t used = 0;
	for(const Entry& entry : entries)
		used += entry.size;
	return used;
}

void EDIScopeQtCUBar::paintEvent(QPaintEvent* /*event*/) {
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, false);

	const double w = width();
	const double h = height();

	// background represents unused CU capacity
	QPainterPath background;
	background.addRoundedRect(QRectF(0, 0, w, h), 4, 4);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.fillPath(background, EDIScopeQtTheme::input_bg);
	painter.setClipPath(background);
	painter.setRenderHint(QPainter::Antialiasing, false);

	QFont count_font = font();
	QFont header_font = font();
	header_font.setPointSizeF(header_font.pointSizeF() * 0.75);

	const Entry* selected_entry = nullptr;
	for(const Entry& entry : entries) {
		double x0 = w * entry.start / total_cus;
		double x1 = w * (entry.start + entry.size) / total_cus;
		if(x1 > w)
			x1 = w;
		if(x0 >= x1)
			continue;

		bool selected = entry.is_audio && entry.subchid == selected_subchid;
		if(selected)
			selected_entry = &entry;

		QRectF rect(x0, 1, x1 - x0, h - 2);
		if(selected) {
			painter.fillRect(rect, EDIScopeQtTheme::accent);
		} else if(entry.is_audio) {
			painter.fillRect(rect, EDIScopeQtTheme::cu_audio);
		} else {
			// data service (e.g. EPG) - not playable: hatched
			painter.fillRect(rect, EDIScopeQtTheme::cu_data);
			painter.fillRect(rect, QBrush(EDIScopeQtTheme::cu_data_hatch, Qt::BDiagPattern));
		}

		painter.setPen(QPen(EDIScopeQtTheme::cu_outline, 1));
		painter.drawRect(rect);

		// SubChId[/SID] (smaller font) above the CU count, clipped to the block so nothing bleeds into the next one
		painter.save();
		painter.setClipRect(rect, Qt::IntersectClip);

		QString header = entry.has_sid ?
				QString::fromStdString(std::to_string(entry.subchid) + " · " + StringTools::SidToHex(entry.sid)) :
				QString::number(entry.subchid);
		QString count = QString::number(entry.size);

		QFontMetricsF header_metrics(header_font);
		QFontMetricsF count_metrics(count_font);
		double content_height = header_metrics.height() + count_metrics.height();
		double y = (h - content_height) / 2;

		QColor text_color = selected ? EDIScopeQtTheme::on_accent : EDIScopeQtTheme::text;
		if(!entry.is_audio)
			text_color = EDIScopeQtTheme::text_secondary;
		painter.setPen(text_color);

		painter.setFont(header_font);
		painter.drawText(QRectF(x0, y, x1 - x0, header_metrics.height()), Qt::AlignHCenter | Qt::AlignTop, header);

		QFont count_font_used = count_font;
		count_font_used.setBold(selected);
		painter.setFont(count_font_used);
		painter.drawText(QRectF(x0, y + header_metrics.height(), x1 - x0, count_metrics.height()), Qt::AlignHCenter | Qt::AlignTop, count);

		painter.restore();
	}

	// the selected block gets a thick outline on top of everything, so it stands out by its shape too
	if(selected_entry) {
		double x0 = w * selected_entry->start / total_cus;
		double x1 = std::min(w, w * (selected_entry->start + selected_entry->size) / total_cus);
		painter.setPen(QPen(EDIScopeQtTheme::text_strong, 3));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(QRectF(x0 + 1.5, 2.5, x1 - x0 - 3, h - 5));
	}
}

void EDIScopeQtCUBar::mousePressEvent(QMouseEvent *event) {
	if(!isEnabled() || event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}

	const Entry* entry = GetEntryAt(event->position().x());
	if(entry && entry->is_audio)
		emit ServiceClicked(entry->sid, entry->scids);
}
