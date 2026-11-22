/*
    DABlin - capital DAB experience
    Copyright (C) 2018-2022 Stefan Pöschel

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

#ifndef DABLIN_GTK_SLS_H_
#define DABLIN_GTK_SLS_H_

#include <deque>
#include <string>

#include <gtkmm.h>

#include "mot_manager.h"



// --- EDIScopeGTKSlideshowContent -----------------------------------------------------------------
// embedded slideshow display; sized to the maximum possible slide dimensions (320x240)
class EDIScopeGTKSlideshowContent : public Gtk::Grid {
public:
	static const int max_slide_width = 320;
	static const int max_slide_height = 240;
private:
	Gtk::EventBox image_event_box;
	Gtk::Image image;
	Gtk::LinkButton link_button;
	Gtk::ProgressBar progress_file;

	Glib::RefPtr<Gdk::Pixbuf> pixbuf_waiting;

	// the raw (still encoded) bytes of the currently shown slide, if any - kept around
	// purely so a right-click can offer saving the exact, original file to disk
	MOT_FILE current_slide;

	// one past slide kept for the history strip: the original encoded bytes (save, tooltip),
	// the full-resolution decoded image (copy, view at normal size) and the scaled-down
	// thumbnail actually shown in the strip
	struct HistorySlide {
		MOT_FILE slide;
		Glib::RefPtr<Gdk::Pixbuf> full_pixbuf;
		Glib::RefPtr<Gdk::Pixbuf> thumb_pixbuf;
		std::string tooltip;
	};

	// vertical strip of thumbnails for the slides shown right before the current one (most
	// recent at the top); sized so the 4 thumbnails plus their gaps together span exactly the
	// same height as the current slide (max_slide_height), regardless of max_history
	static const size_t max_history = 4;
	static const int thumb_spacing = 4;
	static const int thumb_max_height = (max_slide_height - (max_history - 1) * thumb_spacing) / max_history;
	// generous enough that height, not width, is always the binding constraint for the 4:3-ish
	// aspect ratios slides actually come in (see PushHistory)
	static const int thumb_max_width = thumb_max_height * 2;
	Gtk::Box history_box;
	Gtk::EventBox history_event_boxes[max_history];
	Gtk::Image history_images[max_history];
	std::deque<HistorySlide> history;
	void PushHistory(const MOT_FILE& slide, Glib::RefPtr<Gdk::Pixbuf> full_pixbuf);
	void RefreshHistory();
	bool on_history_button_press_event(GdkEventButton* button_event, size_t index);

	// whichever slide (current, or one from the history strip) was right-clicked most
	// recently - captured at click time, so the save/copy/view actions below stay correct
	// even if the slide on screen or the history itself changes while the menu is still open
	MOT_FILE context_slide;
	Glib::RefPtr<Gdk::Pixbuf> context_pixbuf;

	Gtk::Menu slide_context_menu;
	Gtk::MenuItem save_slide_menu_item;
	Gtk::MenuItem copy_slide_menu_item;

	// history thumbnails get the same save/copy, plus a way to see the slide at normal size
	// (they're too small on their own to make out any detail)
	Gtk::Menu thumb_context_menu;
	Gtk::MenuItem thumb_save_menu_item;
	Gtk::MenuItem thumb_copy_menu_item;
	Gtk::MenuItem thumb_view_menu_item;

	bool on_image_button_press_event(GdkEventButton* button_event);
	void on_save_slide_clicked();
	void on_copy_slide_clicked();
	void on_view_slide_clicked();
public:
	EDIScopeGTKSlideshowContent();

	void AwaitSlide();
	void UpdateSlide(const MOT_FILE& slide);
	void UpdateFileProgress(const double fraction);
	void ClearSlide();
	bool IsEmptySlide() {return image.get_storage_type() == Gtk::ImageType::IMAGE_EMPTY;}
};


#endif /* DABLIN_GTK_SLS_H_ */
