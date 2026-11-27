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

#include "ediscope_qt_sls.h"
#include "ediscope_qt_theme.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QToolTip>
#include <QMessageBox>
#include <QScreen>
#include <QVBoxLayout>


static QString GetContentTypeDisplay(int content_sub_type) {
	switch(content_sub_type) {
	case MOT_FILE::CONTENT_SUB_TYPE_JFIF:
		return "JPEG";
	case MOT_FILE::CONTENT_SUB_TYPE_PNG:
		return "PNG";
	default:
		return "unknown";
	}
}

static QString BuildSlideTooltip(const MOT_FILE& slide, const QImage& image) {
	// reception: from its first segment to the complete slide, shown right away
	QString reception;
	if(slide.reception_end.time_since_epoch().count()) {
		QDateTime end = QDateTime::fromMSecsSinceEpoch(std::chrono::duration_cast<std::chrono::milliseconds>(slide.reception_end.time_since_epoch()).count());
		reception = "\nReceived and shown at: " + end.toString("HH:mm:ss");
		if(slide.reception_start.time_since_epoch().count()) {
			double seconds = std::chrono::duration<double>(slide.reception_end - slide.reception_start).count();
			reception += "\nReception time: " + QString::number(seconds, 'f', 1) + " s";
		}
	}

	return QString(
			"Resolution: %1x%2 pixels\n"
			"Size: %3 bytes\n"
			"Format: %4\n"
			"Content name: \"%5\"\n"
			"Content name charset: %6")
			.arg(image.width()).arg(image.height())
			.arg(slide.data.size())
			.arg(GetContentTypeDisplay(slide.content_sub_type))
			.arg(QString::fromStdString(slide.content_name))
			.arg(QString::fromStdString(slide.content_name_charset))
			// hardly any receiver uses it, so it is only shown here rather than as a link below the slide
			+ (slide.click_through_url.empty() ? QString() : "\nClickThroughURL: " + QString::fromStdString(slide.click_through_url))
			+ reception;
}


// --- EDIScopeQtImagePopup -----------------------------------------------------------------
EDIScopeQtImagePopup::EDIScopeQtImagePopup(QWidget *parent) : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint) {
	setAttribute(Qt::WA_ShowWithoutActivating);
	setObjectName("imagePopup");	// coloured by the theme's style sheet
	setAttribute(Qt::WA_StyledBackground);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(4);

	image_label = new QLabel(this);
	text_label = new QLabel(this);

	layout->addWidget(image_label);
	layout->addWidget(text_label);
}

void EDIScopeQtImagePopup::ShowAt(const QPoint& global_pos, const QImage& image, const QString& text) {
	image_label->setPixmap(QPixmap::fromImage(image));
	image_label->setVisible(!image.isNull());	// e.g. text only, while waiting for the first slide
	text_label->setText(text);
	text_label->setVisible(!text.isEmpty());
	adjustSize();
	FollowCursor(global_pos);
	show();
}

void EDIScopeQtImagePopup::FollowCursor(const QPoint& global_pos) {
	// below/right of the pointer, kept on screen
	QPoint pos = global_pos + QPoint(16, 16);
	QScreen *screen = QApplication::screenAt(global_pos);
	if(screen) {
		QRect available = screen->availableGeometry();
		if(pos.x() + width() > available.right())
			pos.setX(global_pos.x() - width() - 16);
		if(pos.y() + height() > available.bottom())
			pos.setY(available.bottom() - height());
		if(pos.y() < available.top())
			pos.setY(available.top());
	}
	move(pos);
}


// --- EDIScopeQtSlideshow -----------------------------------------------------------------
EDIScopeQtSlideshow::EDIScopeQtSlideshow(QWidget *parent) : QWidget(parent), waiting(false), has_current(false) {
	QGridLayout *layout = new QGridLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setHorizontalSpacing(8);
	layout->setVerticalSpacing(6);

	// a slide is at most 320x240 pixels; reserve exactly that so the box neither grows nor shrinks with slides
	image_label = new QLabel(this);
	image_label->setFixedSize(max_slide_width, max_slide_height);
	image_label->setAlignment(Qt::AlignCenter);
	image_label->setContextMenuPolicy(Qt::CustomContextMenu);
	image_label->installEventFilter(this);	// hovering shows the slide's popup, as for the history thumbnails
	image_label->setMouseTracking(true);	// so the popup follows the pointer
	image_label->setObjectName("slideImage");
	connect(image_label, &QLabel::customContextMenuRequested, this, [this](const QPoint& pos) {
		popup->hide();
		if(has_current)
			ShowContextMenu(current, image_label->mapToGlobal(pos), false);
	});
	layout->addWidget(image_label, 0, 0);

	QVBoxLayout *history_layout = new QVBoxLayout();
	history_layout->setSpacing(thumb_spacing);
	history_layout->setContentsMargins(0, 0, 0, 0);
	for(size_t i = 0; i < max_history; i++) {
		history_labels[i] = new QLabel(this);
		history_labels[i]->setFixedSize(thumb_max_width, thumb_max_height);
		history_labels[i]->setAlignment(Qt::AlignLeft | Qt::AlignTop);
		history_labels[i]->setContextMenuPolicy(Qt::CustomContextMenu);
		history_labels[i]->installEventFilter(this);
		history_labels[i]->setMouseTracking(true);	// so the popup follows the pointer
		connect(history_labels[i], &QLabel::customContextMenuRequested, this, [this, i](const QPoint& pos) {
			if(i < history.size()) {
				popup->hide();
				ShowContextMenu(history[i], history_labels[i]->mapToGlobal(pos), true);
			}
		});
		history_layout->addWidget(history_labels[i]);
	}
	history_layout->addStretch(1);
	layout->addLayout(history_layout, 0, 1);

	progress_file = new QProgressBar(this);
	progress_file->setRange(0, 1000);
	progress_file->setTextVisible(false);
	progress_file->setMaximumWidth(max_slide_width);
	// half the height of the regular progress bars (14px)
	progress_file->setFixedHeight(7);
	progress_file->setStyleSheet("QProgressBar { border-radius: 3px; max-height: 7px; } QProgressBar::chunk { border-radius: 2px; }");
	progress_file->setToolTip("Reception progress of the next slide");
	// always shown (empty while nothing is being received), so the panel never changes height
	progress_file->setValue(0);
	layout->addWidget(progress_file, 1, 0, 1, 2);

	popup = new EDIScopeQtImagePopup(this);
	popup->hide();

	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void EDIScopeQtSlideshow::AwaitSlide() {
	QPixmap placeholder(max_slide_width, max_slide_height);
	placeholder.fill(EDIScopeQtTheme::slide_waiting);
	image_label->setPixmap(placeholder);
	waiting = true;

	ResetFileProgress();

	// a fresh wait (new service, or the very first slide) - any previously shown slides are
	// from a different context by now, so they no longer belong in the history strip
	has_current = false;
	current = Slide();
	history.clear();
	RefreshHistory();
}

void EDIScopeQtSlideshow::UpdateSlide(const MOT_FILE& slide) {
	const char *format = nullptr;
	switch(slide.content_sub_type) {
	case MOT_FILE::CONTENT_SUB_TYPE_JFIF:
		format = "JPEG";
		break;
	case MOT_FILE::CONTENT_SUB_TYPE_PNG:
		format = "PNG";
		break;
	}

	QImage image;
	if(!image.loadFromData(slide.data.data(), (int) slide.data.size(), format))
		return;

	// push the slide currently on screen into the history strip, before replacing it - skip
	// while nothing but the "waiting for slide" placeholder is shown
	if(has_current) {
		history.push_front(current);
		while(history.size() > max_history)
			history.pop_back();
		RefreshHistory();
	}

	current.file = slide;
	current.image = image;
	current.tooltip = BuildSlideTooltip(slide, image);
	has_current = true;
	waiting = false;

	image_label->setPixmap(QPixmap::fromImage(image));
	// a new slide while the pointer is on it: refresh the popup already shown
	if(popup->isVisible() && image_label->underMouse())
		popup->ShowAt(QCursor::pos(), current.image, current.tooltip);

	// the slide is complete: empty the file progress again
	ResetFileProgress();
}

void EDIScopeQtSlideshow::UpdateFileProgress(double fraction) {
	if(fraction == -1) {
		progress_file->setRange(0, 0);	// unknown progress: busy indicator
	} else {
		progress_file->setRange(0, 1000);
		progress_file->setValue((int) (fraction * 1000));
	}
}

void EDIScopeQtSlideshow::ResetFileProgress() {
	progress_file->setRange(0, 1000);
	progress_file->setValue(0);
}

void EDIScopeQtSlideshow::ClearSlide() {
	image_label->clear();
	popup->hide();
	waiting = false;
	has_current = false;
	current = Slide();
	history.clear();
	RefreshHistory();
	ResetFileProgress();
}

void EDIScopeQtSlideshow::RefreshHistory() {
	for(size_t i = 0; i < max_history; i++) {
		if(i < history.size())
			history_labels[i]->setPixmap(QPixmap::fromImage(history[i].image.scaled(thumb_max_width, thumb_max_height, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
		else
			history_labels[i]->clear();
	}
}

bool EDIScopeQtSlideshow::eventFilter(QObject *watched, QEvent *event) {
	// hovering the slide shows the same popup as a history thumbnail: the slide with its information
	if(watched == image_label) {
		switch(event->type()) {
		case QEvent::Enter:
			if(has_current)
				popup->ShowAt(QCursor::pos(), current.image, current.tooltip);
			else if(waiting)
				popup->ShowAt(QCursor::pos(), QImage(), "Waiting for slide...");
			break;
		case QEvent::MouseMove:
			if(popup->isVisible())
				popup->FollowCursor(QCursor::pos());
			break;
		case QEvent::Leave:
		case QEvent::Hide:
			popup->hide();
			break;
		case QEvent::ToolTip:
			return true;	// the popup replaces the regular tooltip
		default:
			break;
		}
		return QWidget::eventFilter(watched, event);
	}

	for(size_t i = 0; i < max_history; i++) {
		if(watched != history_labels[i])
			continue;

		switch(event->type()) {
		case QEvent::Enter:
			if(i < history.size())
				popup->ShowAt(QCursor::pos(), history[i].image, history[i].tooltip);
			break;
		case QEvent::MouseMove:
			if(popup->isVisible())
				popup->FollowCursor(QCursor::pos());
			break;
		case QEvent::Leave:
		case QEvent::Hide:
			popup->hide();
			break;
		case QEvent::ToolTip:
			return true;	// the popup replaces the regular tooltip
		default:
			break;
		}
		break;
	}
	return QWidget::eventFilter(watched, event);
}

void EDIScopeQtSlideshow::ShowContextMenu(const Slide& slide, const QPoint& global_pos, bool offer_view) {
	// captured by value, so the actions stay correct even if the slide on screen or the history
	// itself changes while the menu is still open
	Slide context_slide = slide;

	QMenu menu(this);
	menu.addAction("Save slide as...", this, [this, context_slide]() {SaveSlide(context_slide);});
	menu.addAction("Copy", this, [context_slide]() {QApplication::clipboard()->setImage(context_slide.image);});
	if(offer_view)
		menu.addAction("View at normal size", this, [this, context_slide]() {ViewSlide(context_slide);});
	menu.exec(global_pos);
}

void EDIScopeQtSlideshow::SaveSlide(const Slide& slide) {
	if(slide.file.data.empty())
		return;

	QString extension;
	switch(slide.file.content_sub_type) {
	case MOT_FILE::CONTENT_SUB_TYPE_JFIF:
		extension = ".jpg";
		break;
	case MOT_FILE::CONTENT_SUB_TYPE_PNG:
		extension = ".png";
		break;
	}
	QString suggested_name = !slide.file.content_name.empty() ? QString::fromStdString(slide.file.content_name) : "slide" + extension;

	QString filename = QFileDialog::getSaveFileName(this, "Save slide as...", suggested_name);
	if(filename.isEmpty())
		return;

	// the exact, original file
	QFile file(filename);
	if(!file.open(QIODevice::WriteOnly) || file.write((const char*) slide.file.data.data(), (qint64) slide.file.data.size()) != (qint64) slide.file.data.size())
		QMessageBox::critical(this, "EDIScope", "Could not save the slide:\n" + file.errorString());
}

void EDIScopeQtSlideshow::ViewSlide(const Slide& slide) {
	QDialog dialog(this);
	dialog.setWindowTitle("Slide");

	QVBoxLayout *layout = new QVBoxLayout(&dialog);
	QLabel *image = new QLabel(&dialog);
	image->setPixmap(QPixmap::fromImage(slide.image));
	layout->addWidget(image);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);

	layout->setSizeConstraint(QLayout::SetFixedSize);
	dialog.exec();
}
