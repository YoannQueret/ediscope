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

#ifndef EDISCOPE_QT_SLS_H_
#define EDISCOPE_QT_SLS_H_

#include <deque>
#include <string>

#include <QImage>
#include <QLabel>
#include <QProgressBar>
#include <QWidget>

#include "mot_manager.h"


// --- EDIScopeQtImagePopup -----------------------------------------------------------------
// a tooltip-like popup showing an image above a few lines of text - shown on hover over a slide
// history thumbnail, too small on its own to make out any detail
class EDIScopeQtImagePopup : public QWidget {
	Q_OBJECT
private:
	QLabel *image_label;
	QLabel *text_label;
public:
	EDIScopeQtImagePopup(QWidget *parent = nullptr);
	// shows the popup next to the pointer - either may be empty (text only, image only)
	void ShowAt(const QPoint& global_pos, const QImage& image, const QString& text);
	// keeps an already shown popup next to the pointer as it moves
	void FollowCursor(const QPoint& global_pos);
};


// --- EDIScopeQtSlideshow -----------------------------------------------------------------
// embedded slideshow display; sized to the maximum possible slide dimensions (320x240), with a
// vertical strip of the slides shown right before the current one (most recent at the top)
class EDIScopeQtSlideshow : public QWidget {
	Q_OBJECT
public:
	static const int max_slide_width = 320;
	static const int max_slide_height = 240;
private:
	static const size_t max_history = 4;
	static const int thumb_spacing = 4;
	// the 4 thumbnails plus their gaps together span exactly the current slide's height
	static const int thumb_max_height = (max_slide_height - (max_history - 1) * thumb_spacing) / max_history;
	// generous enough that height, not width, is always the binding constraint
	static const int thumb_max_width = thumb_max_height * 2;

	struct Slide {
		MOT_FILE file;	// the original, still encoded bytes (save, tooltip)
		QImage image;	// decoded, full resolution (copy, view at normal size)
		QString tooltip;
	};

	QLabel *image_label;
	QLabel *history_labels[max_history];
	QProgressBar *progress_file;
	EDIScopeQtImagePopup *popup;

	bool waiting;	// "waiting for slide" placeholder shown
	bool has_current;
	Slide current;
	std::deque<Slide> history;

	void RefreshHistory();
	void ResetFileProgress();
	void ShowContextMenu(const Slide& slide, const QPoint& global_pos, bool offer_view);
	void SaveSlide(const Slide& slide);
	void ViewSlide(const Slide& slide);
protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
public:
	EDIScopeQtSlideshow(QWidget *parent = nullptr);

	void AwaitSlide();
	void UpdateSlide(const MOT_FILE& slide);
	void UpdateFileProgress(double fraction);
	void ClearSlide();
	bool IsEmptySlide() const {return !waiting && !has_current;}
};

#endif /* EDISCOPE_QT_SLS_H_ */
