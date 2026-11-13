/*
    DABlin - capital DAB experience
    Copyright (C) 2018-2022 Stefan Pöschel

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

#include "ediscope_gtk_sls.h"

#include <algorithm>
#include <cmath>
#include <fstream>


static std::string GetContentTypeDisplay(int content_sub_type) {
	switch(content_sub_type) {
	case MOT_FILE::CONTENT_SUB_TYPE_JFIF:
		return "JPEG";
	case MOT_FILE::CONTENT_SUB_TYPE_PNG:
		return "PNG";
	default:
		return "unknown";
	}
}

static std::string BuildSlideTooltip(const MOT_FILE& slide, const Glib::RefPtr<Gdk::Pixbuf>& pixbuf) {
	return
			"Resolution: " + std::to_string(pixbuf->get_width()) + "x" + std::to_string(pixbuf->get_height()) + " pixels\n"
			"Size: " + std::to_string(slide.data.size()) + " bytes\n"
			"Format: " + GetContentTypeDisplay(slide.content_sub_type) + "\n"
			"Content name: \"" + slide.content_name + "\"\n"
			"Content name charset: " + slide.content_name_charset;
}


// --- EDIScopeGTKSlideshowContent -----------------------------------------------------------------
EDIScopeGTKSlideshowContent::EDIScopeGTKSlideshowContent() {
	pixbuf_waiting = Gdk::Pixbuf::create(Gdk::COLORSPACE_RGB, false, 8, max_slide_width, max_slide_height);
	pixbuf_waiting->fill(0x003000FF);

	// a slide is at most 320x240 pixels; reserve exactly that so the box neither grows nor shrinks with slides
	image.set_size_request(max_slide_width, max_slide_height);
	image.set_margin_start(5);
	image.set_margin_end(5);
	image.set_margin_bottom(5);

	// GtkImage has no window of its own, so wrap it to reliably receive the right-click
	image_event_box.add(image);
	image_event_box.add_events(Gdk::BUTTON_PRESS_MASK);
	image_event_box.signal_button_press_event().connect(sigc::mem_fun(*this, &EDIScopeGTKSlideshowContent::on_image_button_press_event));

	save_slide_menu_item.set_label("Save slide as...");
	save_slide_menu_item.signal_activate().connect(sigc::mem_fun(*this, &EDIScopeGTKSlideshowContent::on_save_slide_clicked));
	slide_context_menu.append(save_slide_menu_item);

	copy_slide_menu_item.set_label("Copy");
	copy_slide_menu_item.signal_activate().connect(sigc::mem_fun(*this, &EDIScopeGTKSlideshowContent::on_copy_slide_clicked));
	slide_context_menu.append(copy_slide_menu_item);

	slide_context_menu.show_all();

	thumb_save_menu_item.set_label("Save slide as...");
	thumb_save_menu_item.signal_activate().connect(sigc::mem_fun(*this, &EDIScopeGTKSlideshowContent::on_save_slide_clicked));
	thumb_context_menu.append(thumb_save_menu_item);

	thumb_copy_menu_item.set_label("Copy");
	thumb_copy_menu_item.signal_activate().connect(sigc::mem_fun(*this, &EDIScopeGTKSlideshowContent::on_copy_slide_clicked));
	thumb_context_menu.append(thumb_copy_menu_item);

	thumb_view_menu_item.set_label("View at normal size");
	thumb_view_menu_item.signal_activate().connect(sigc::mem_fun(*this, &EDIScopeGTKSlideshowContent::on_view_slide_clicked));
	thumb_context_menu.append(thumb_view_menu_item);

	thumb_context_menu.show_all();

	history_box.set_orientation(Gtk::ORIENTATION_VERTICAL);
	history_box.set_spacing(thumb_spacing);
	for(size_t i = 0; i < max_history; i++) {
		history_images[i].set_size_request(thumb_max_width, thumb_max_height);
		history_images[i].set_valign(Gtk::ALIGN_START);

		// GtkImage has no window of its own, so wrap it to reliably receive the right-click
		history_event_boxes[i].add(history_images[i]);
		history_event_boxes[i].add_events(Gdk::BUTTON_PRESS_MASK);
		history_event_boxes[i].signal_button_press_event().connect(
				sigc::bind(sigc::mem_fun(*this, &EDIScopeGTKSlideshowContent::on_history_button_press_event), i));

		history_box.pack_start(history_event_boxes[i], Gtk::PACK_SHRINK);
	}

	attach(image_event_box, 0, 0, 1, 1);
	attach_next_to(history_box, image_event_box, Gtk::POS_RIGHT, 1, 1);
	attach_next_to(link_button, image_event_box, Gtk::POS_BOTTOM, 1, 1);
	attach_next_to(progress_file, link_button, Gtk::POS_BOTTOM, 1, 1);
}

void EDIScopeGTKSlideshowContent::AwaitSlide() {
	image.set(pixbuf_waiting);
	image.set_tooltip_text("Waiting for slide...");

	link_button.hide();
	progress_file.hide();

	// a fresh wait (new service, or the very first slide) - any previously shown slides are
	// from a different context by now, so they no longer belong in the history strip
	current_slide = MOT_FILE();
	history.clear();
	RefreshHistory();
}

void EDIScopeGTKSlideshowContent::UpdateSlide(const MOT_FILE& slide) {
	std::string type_mime;
	switch(slide.content_sub_type) {
	case MOT_FILE::CONTENT_SUB_TYPE_JFIF:
		type_mime = "image/jpeg";
		break;
	case MOT_FILE::CONTENT_SUB_TYPE_PNG:
		type_mime = "image/png";
		break;
	}

	Glib::RefPtr<Gdk::PixbufLoader> pixbuf_loader = Gdk::PixbufLoader::create(type_mime, true);
	pixbuf_loader->write(&slide.data[0], slide.data.size());
	pixbuf_loader->close();

	Glib::RefPtr<Gdk::Pixbuf> pixbuf = pixbuf_loader->get_pixbuf();
	if(!pixbuf)
		return;

	// push the slide currently on screen into the history strip, before replacing it - skip
	// while nothing but the "waiting for slide" placeholder is shown (current_slide empty)
	if(!current_slide.data.empty())
		PushHistory(current_slide, image.get_pixbuf());

	// keep the raw, still encoded bytes around, so a right-click can offer saving the exact original file
	current_slide = slide;

	// update image
	image.set(pixbuf);
	image.set_tooltip_text(BuildSlideTooltip(slide, pixbuf));

	// update ClickThroughURL link
	if(!slide.click_through_url.empty()) {
		link_button.set_label(slide.click_through_url);
		link_button.set_tooltip_text(slide.click_through_url);
		link_button.set_uri(slide.click_through_url);

		// ensure that the label (created/replaced by set_label) does not extend the slide
		Gtk::Label* l = (Gtk::Label*) link_button.get_child();
		l->set_max_width_chars(1);
		l->set_ellipsize(Pango::ELLIPSIZE_END);

		link_button.show();
	} else {
		link_button.hide();
	}

	// hide file progress
	progress_file.hide();
}

void EDIScopeGTKSlideshowContent::UpdateFileProgress(const double fraction) {
	// update/show file progress
	if(fraction == -1)
		progress_file.pulse();	// unknown progress
	else
		progress_file.set_fraction(fraction);
	progress_file.show();
}

void EDIScopeGTKSlideshowContent::ClearSlide() {
	image.clear();
	current_slide = MOT_FILE();
	history.clear();
	RefreshHistory();
}

void EDIScopeGTKSlideshowContent::PushHistory(const MOT_FILE& slide, Glib::RefPtr<Gdk::Pixbuf> full_pixbuf) {
	if(!full_pixbuf)
		return;

	HistorySlide entry;
	entry.slide = slide;
	entry.full_pixbuf = full_pixbuf;
	entry.tooltip = BuildSlideTooltip(slide, full_pixbuf);

	// scale down to thumbnail size, preserving aspect ratio
	double scale = std::min((double) thumb_max_width / full_pixbuf->get_width(), (double) thumb_max_height / full_pixbuf->get_height());
	int w = std::max(1, (int) std::lround(full_pixbuf->get_width() * scale));
	int h = std::max(1, (int) std::lround(full_pixbuf->get_height() * scale));
	entry.thumb_pixbuf = full_pixbuf->scale_simple(w, h, Gdk::INTERP_BILINEAR);

	history.push_front(entry);
	while(history.size() > max_history)
		history.pop_back();

	RefreshHistory();
}

void EDIScopeGTKSlideshowContent::RefreshHistory() {
	size_t i = 0;
	for(; i < history.size(); i++) {
		history_images[i].set(history[i].thumb_pixbuf);
		history_images[i].set_tooltip_text(history[i].tooltip);
	}
	for(; i < max_history; i++) {
		history_images[i].clear();
		history_images[i].set_tooltip_text("");
	}
}

bool EDIScopeGTKSlideshowContent::on_image_button_press_event(GdkEventButton* button_event) {
	if(button_event->type != GDK_BUTTON_PRESS || button_event->button != 3)
		return false;
	if(current_slide.data.empty())
		return false;

	context_slide = current_slide;
	context_pixbuf = image.get_pixbuf();
	slide_context_menu.popup_at_pointer((GdkEvent*) button_event);
	return true;
}

bool EDIScopeGTKSlideshowContent::on_history_button_press_event(GdkEventButton* button_event, size_t index) {
	if(button_event->type != GDK_BUTTON_PRESS || button_event->button != 3)
		return false;
	if(index >= history.size())
		return false;

	context_slide = history[index].slide;
	context_pixbuf = history[index].full_pixbuf;
	thumb_context_menu.popup_at_pointer((GdkEvent*) button_event);
	return true;
}

void EDIScopeGTKSlideshowContent::on_save_slide_clicked() {
	if(context_slide.data.empty())
		return;

	std::string extension;
	switch(context_slide.content_sub_type) {
	case MOT_FILE::CONTENT_SUB_TYPE_JFIF:
		extension = ".jpg";
		break;
	case MOT_FILE::CONTENT_SUB_TYPE_PNG:
		extension = ".png";
		break;
	default:
		extension = "";
		break;
	}

	std::string suggested_name = !context_slide.content_name.empty() ? context_slide.content_name : "slide" + extension;

	Gtk::Window* parent = (Gtk::Window*) get_toplevel();
	Gtk::FileChooserDialog dialog(*parent, "Save slide as...", Gtk::FILE_CHOOSER_ACTION_SAVE);
	dialog.set_do_overwrite_confirmation(true);
	dialog.set_current_name(suggested_name);
	dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
	dialog.add_button("_Save", Gtk::RESPONSE_OK);

	if(dialog.run() != Gtk::RESPONSE_OK)
		return;

	std::string filename = dialog.get_filename();

	std::ofstream file(filename, std::ios::binary);
	file.write((const char*) context_slide.data.data(), context_slide.data.size());
}

void EDIScopeGTKSlideshowContent::on_copy_slide_clicked() {
	if(!context_pixbuf)
		return;

	Gtk::Clipboard::get()->set_image(context_pixbuf);
}

void EDIScopeGTKSlideshowContent::on_view_slide_clicked() {
	if(!context_pixbuf)
		return;

	Gtk::Window* parent = (Gtk::Window*) get_toplevel();
	Gtk::Dialog dialog("Slide", *parent);
	dialog.set_resizable(false);
	dialog.add_button("_Close", Gtk::RESPONSE_CLOSE);

	Gtk::Image *img = Gtk::manage(new Gtk::Image(context_pixbuf));
	img->set_margin_start(10);
	img->set_margin_end(10);
	img->set_margin_top(10);
	img->set_margin_bottom(10);
	dialog.get_content_area()->pack_start(*img);

	dialog.show_all();
	dialog.run();
}
