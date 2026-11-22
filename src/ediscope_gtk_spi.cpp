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

#include "ediscope_gtk_spi.h"

#include <algorithm>
#include <ctime>
#include <iterator>

#include "tools.h"


static const char *SPI_CSS = R"CSS(
.spi-scroll {
	background-color: #eef1f4;
}
.spi-card {
	background-color: #ffffff;
	border: 1px solid #e3e6ea;
	border-radius: 10px;
	padding: 12px;
}
.spi-name {
	font-weight: 700;
	font-size: 14px;
	color: #20242c;
}
.spi-bearer {
	font-family: monospace;
	font-size: 10.5px;
	color: #8d94a0;
}
.spi-logo-box {
	background-color: #f6f7f9;
	border: 1px solid #e7e9ec;
	border-radius: 6px;
	padding: 6px;
}
.spi-logo-caption {
	font-size: 10px;
	color: #9299a5;
}
.spi-empty {
	color: #9299a5;
	font-size: 13px;
}
.spi-xml-view {
	font-family: monospace;
	font-size: 11px;
}
.spi-group-type {
	font-size: 10.5px;
	color: #8d94a0;
}
.spi-toolbar {
	background-color: #ffffff;
	border-bottom: 1px solid #e3e6ea;
	padding: 6px;
}
.spi-programme-block {
	background-color: #4a7fe8;
	border-radius: 6px;
	padding: 3px 8px;
}
.spi-programme-name {
	color: #ffffff;
	font-weight: 600;
	font-size: 11px;
}
.spi-programme-time {
	color: #dbe6ff;
	font-size: 9.5px;
}
.spi-hour-label {
	font-size: 10px;
	color: #9299a5;
}
.spi-hour-line {
	background-color: #e3e6ea;
}
)CSS";


// --- EDIScopeGTKSPIWindow -----------------------------------------------------------------
EDIScopeGTKSPIWindow::EDIScopeGTKSPIWindow() :
		logos_page(Gtk::ORIENTATION_VERTICAL),
		programme_page(Gtk::ORIENTATION_VERTICAL),
		groupes_page(Gtk::ORIENTATION_VERTICAL),
		xml_page(Gtk::ORIENTATION_VERTICAL)
{
	positioned = false;

	set_title("SPI");
	set_type_hint(Gdk::WINDOW_TYPE_HINT_UTILITY);
	// deletable (unlike e.g. the logs window): this one tends to end up behind the main window,
	// where re-finding the small SPI toggle button to close it again is a hassle - closing via
	// the window manager's own close button is handled (kept alive, just hidden) by EDIScopeGTK,
	// which also owns the toggle button that must stay in sync with it

	css_provider = Gtk::CssProvider::create();
	css_provider->load_from_data(SPI_CSS);
	Gtk::StyleContext::add_provider_for_screen(Gdk::Screen::get_default(), css_provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	// --- "Services" tab (SI) ---
	empty_label.set_text("No SPI service detected on this ensemble yet.");
	empty_label.get_style_context()->add_class("spi-empty");
	empty_label.set_margin_top(24);
	empty_label.set_line_wrap(true);

	// a fixed number of columns (rather than however many happen to fit the window width) keeps
	// the grid predictable, and lets the default window size be computed to show logos_rows_on_open
	// full rows of it without having to scroll
	content_flow.set_selection_mode(Gtk::SELECTION_NONE);
	content_flow.set_homogeneous(false);
	// deliberately no set_min_children_per_line(): with fewer cards than that minimum (e.g. a
	// partial last row, or just few services overall), FlowBox renders the line with zero
	// height instead of simply left-aligning what it has - max alone still caps wrapping at
	// logos_per_row per line
	content_flow.set_max_children_per_line(logos_per_row);
	content_flow.set_row_spacing(10);
	content_flow.set_column_spacing(10);
	content_flow.set_margin_start(10);
	content_flow.set_margin_end(10);
	content_flow.set_margin_top(10);
	content_flow.set_margin_bottom(10);
	content_flow.set_valign(Gtk::ALIGN_START);

	scroll.get_style_context()->add_class("spi-scroll");
	scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
	scroll.add(content_flow);

	logos_page.pack_start(empty_label, false, false);
	logos_page.pack_start(scroll, true, true);

	// --- "Programme" tab (PI) ---
	Gtk::Box *programme_toolbar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
	programme_toolbar->get_style_context()->add_class("spi-toolbar");
	programme_service_selector.signal_changed().connect(sigc::mem_fun(*this, &EDIScopeGTKSPIWindow::on_programme_service_changed));
	programme_toolbar->pack_start(programme_service_selector, false, false);

	programme_empty_label.set_text("No programme schedule decoded yet for this ensemble.");
	programme_empty_label.get_style_context()->add_class("spi-empty");
	programme_empty_label.set_margin_top(24);
	programme_empty_label.set_line_wrap(true);

	programme_timeline.set_margin_start(10);
	programme_timeline.set_margin_top(10);

	programme_scroll.get_style_context()->add_class("spi-scroll");
	programme_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
	programme_scroll.add(programme_timeline);

	programme_page.pack_start(*programme_toolbar, false, false);
	programme_page.pack_start(programme_empty_label, false, false);
	programme_page.pack_start(programme_scroll, true, true);

	// --- "Groupes" tab (GI) ---
	groupes_empty_label.set_text("No programme group decoded yet for this ensemble.");
	groupes_empty_label.get_style_context()->add_class("spi-empty");
	groupes_empty_label.set_margin_top(24);
	groupes_empty_label.set_line_wrap(true);

	groupes_flow.set_selection_mode(Gtk::SELECTION_NONE);
	groupes_flow.set_homogeneous(false);
	groupes_flow.set_max_children_per_line(logos_per_row);
	groupes_flow.set_row_spacing(10);
	groupes_flow.set_column_spacing(10);
	groupes_flow.set_margin_start(10);
	groupes_flow.set_margin_end(10);
	groupes_flow.set_margin_top(10);
	groupes_flow.set_margin_bottom(10);
	groupes_flow.set_valign(Gtk::ALIGN_START);

	groupes_scroll.get_style_context()->add_class("spi-scroll");
	groupes_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
	groupes_scroll.add(groupes_flow);

	groupes_page.pack_start(groupes_empty_label, false, false);
	groupes_page.pack_start(groupes_scroll, true, true);

	// --- "XML" tab ---
	Gtk::Box *xml_toolbar = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL));
	xml_toolbar->get_style_context()->add_class("spi-toolbar");
	xml_selector.append("Services");
	xml_selector.append("Programme");
	xml_selector.append("Groupes");
	xml_selector.set_active(0);
	xml_selector.signal_changed().connect(sigc::mem_fun(*this, &EDIScopeGTKSPIWindow::on_xml_selector_changed));
	xml_toolbar->pack_start(xml_selector, false, false);

	xml_textview.get_style_context()->add_class("spi-xml-view");
	xml_textview.set_editable(false);
	xml_textview.set_monospace(true);
	xml_textview.set_left_margin(10);
	xml_textview.set_top_margin(10);
	xml_buffer = xml_textview.get_buffer();

	xml_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	xml_scroll.add(xml_textview);

	xml_page.pack_start(*xml_toolbar, false, false);
	xml_page.pack_start(xml_scroll, true, true);

	notebook.append_page(logos_page, "Services");
	notebook.append_page(programme_page, "Programme");
	notebook.append_page(groupes_page, "Groupes");
	notebook.append_page(xml_page, "XML");
	add(notebook);

	// card width (set on each card below) * logos_per_row, plus the flow's own spacing/margins
	// and a card's own logo row (4 thumbnails wide) - both drive the same column count, so the
	// window ends up wide enough for both without a horizontal scrollbar
	static const int card_width = 280;
	static const int card_height = 260;
	int width = 10 + 10 + logos_per_row * card_width + (logos_per_row - 1) * 10 + 20;	// + scrollbar allowance
	int height = 10 + 10 + logos_rows_on_open * card_height + (logos_rows_on_open - 1) * 10 + 60;	// + tab bar allowance
	set_default_size(width, height);

	show_all_children();
	scroll.hide();
	programme_scroll.hide();
	groupes_scroll.hide();
}

EDIScopeGTKSPIWindow::~EDIScopeGTKSPIWindow() {
	ClearContent();
}

void EDIScopeGTKSPIWindow::AlignToParent() {
	// only once, the first time the window is shown - afterwards it stays wherever the user left
	// it (moved or not), it doesn't stick to / follow the main window around
	if(positioned)
		return;
	positioned = true;

	int x, y, w, h;
	get_transient_for()->get_position(x, y);
	get_transient_for()->get_size(w, h);

	move(x + w + 20, y);
}

std::string EDIScopeGTKSPIWindow::PickName(const std::map<std::string,std::string>& names) {
	if(names.empty())
		return "";

	std::map<std::string,std::string>::const_iterator it = names.find("");
	return it != names.end() ? it->second : names.begin()->second;
}

std::string EDIScopeGTKSPIWindow::FormatDABBearer(bool has_dab_bearer, int ecc, int eid, uint32_t sid, bool sid_32bit, int scids) {
	if(!has_dab_bearer)
		return "(no DAB bearer)";

	return "dab:" +
			StringTools::IntToHex(ecc, 2) + "." +
			StringTools::IntToHex(eid, 4) + "." +
			StringTools::IntToHex((int) sid, sid_32bit ? 8 : 4) + "." +
			std::to_string(scids);
}

std::string EDIScopeGTKSPIWindow::FormatLogoCaption(const SPI_LOGO& logo) {
	// logo_colour_square/logo_colour_rectangle have a mandatory fixed size (ETSI TS 102 818
	// clause 5.8) and must NOT carry width/height attributes at all - only logo_unrestricted
	// actually signals its own dimensions
	switch(logo.type) {
	case SPI_LOGO::LOGO_TYPE_COLOUR_SQUARE:
		return "square 32x32";
	case SPI_LOGO::LOGO_TYPE_COLOUR_RECTANGLE:
		return "rectangle 112x32";
	case SPI_LOGO::LOGO_TYPE_UNRESTRICTED: {
		std::string dims = logo.width > 0 && logo.height > 0 ? " " + std::to_string(logo.width) + "x" + std::to_string(logo.height) : "";
		return "unrestricted" + dims;
	}
	default:
		return "logo";
	}
}

std::string EDIScopeGTKSPIWindow::FindServiceName(uint32_t sid) const {
	for(const SPI_SERVICE& service : last_si.services) {
		if(!service.has_dab_bearer || service.sid != sid)
			continue;

		std::string name = PickName(service.long_names);
		if(name.empty())
			name = PickName(service.medium_names);
		if(name.empty())
			name = PickName(service.short_names);
		if(!name.empty())
			return name;
	}

	std::map<uint32_t,std::string>::const_iterator it = known_service_names.find(sid);
	if(it != known_service_names.end())
		return it->second;

	return "SId " + StringTools::SidToHex((int) sid);
}

void EDIScopeGTKSPIWindow::SetKnownServiceNames(const std::map<uint32_t,std::string>& names) {
	known_service_names = names;
	RebuildProgrammeSelector();
}

Glib::RefPtr<Gdk::Pixbuf> EDIScopeGTKSPIWindow::LoadLogoPixbuf(const SPI_LOGO& logo, const std::map<std::string,std::vector<uint8_t>>& logo_data) {
	std::map<std::string,std::vector<uint8_t>>::const_iterator it = logo_data.find(logo.content_name);
	if(it == logo_data.end() || it->second.empty())
		return Glib::RefPtr<Gdk::Pixbuf>();

	try {
		Glib::RefPtr<Gdk::PixbufLoader> loader = Gdk::PixbufLoader::create();
		loader->write(&it->second[0], it->second.size());
		loader->close();
		return loader->get_pixbuf();
	} catch(const Glib::Error& e) {
		fprintf(stderr, "EDIScopeGTKSPIWindow: failed to decode logo '%s': %s\n", logo.content_name.c_str(), e.what().c_str());
		return Glib::RefPtr<Gdk::Pixbuf>();
	}
}

Glib::RefPtr<Gdk::Pixbuf> EDIScopeGTKSPIWindow::ScaleToThumbnail(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf) {
	static const int max_thumb = 72;
	int w = pixbuf->get_width();
	int h = pixbuf->get_height();
	if(w <= max_thumb && h <= max_thumb)
		return pixbuf;

	double scale = (double) max_thumb / (w > h ? w : h);
	int new_w = (int) (w * scale);
	int new_h = (int) (h * scale);
	if(new_w < 1)
		new_w = 1;
	if(new_h < 1)
		new_h = 1;

	return pixbuf->scale_simple(new_w, new_h, Gdk::INTERP_BILINEAR);
}

bool EDIScopeGTKSPIWindow::OnLogoQueryTooltip(int /*x*/, int /*y*/, bool /*keyboard_tooltip*/, const Glib::RefPtr<Gtk::Tooltip>& tooltip, Glib::RefPtr<Gdk::Pixbuf> full_pixbuf) {
	// shows the logo at its real, undownscaled size - a plain Gtk::Image tooltip "just works" for
	// this, no custom popup window needed
	Gtk::Image *full_image = Gtk::manage(new Gtk::Image(full_pixbuf));
	tooltip->set_custom(*full_image);
	return true;
}

Gtk::Widget *EDIScopeGTKSPIWindow::BuildLogoBox(const SPI_LOGO& logo, const std::map<std::string,std::vector<uint8_t>>& logo_data) {
	Gtk::Box *logo_box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
	logo_box->get_style_context()->add_class("spi-logo-box");

	Gtk::Image *image = Gtk::manage(new Gtk::Image());
	Glib::RefPtr<Gdk::Pixbuf> full_pixbuf = LoadLogoPixbuf(logo, logo_data);
	if(full_pixbuf) {
		image->set(ScaleToThumbnail(full_pixbuf));
		logo_box->set_has_tooltip(true);
		logo_box->signal_query_tooltip().connect(sigc::bind(sigc::ptr_fun(&EDIScopeGTKSPIWindow::OnLogoQueryTooltip), full_pixbuf));
	} else {
		image->set_from_icon_name("image-missing", Gtk::ICON_SIZE_DIALOG);
	}
	image->set_halign(Gtk::ALIGN_CENTER);

	Gtk::Label *caption = Gtk::manage(new Gtk::Label(FormatLogoCaption(logo)));
	caption->get_style_context()->add_class("spi-logo-caption");
	caption->set_justify(Gtk::JUSTIFY_CENTER);
	caption->set_halign(Gtk::ALIGN_CENTER);

	logo_box->pack_start(*image, false, false);
	logo_box->pack_start(*caption, false, false);
	return logo_box;
}

Gtk::Widget *EDIScopeGTKSPIWindow::BuildServiceCard(const SPI_SERVICE& service, const std::map<std::string,std::vector<uint8_t>>& logo_data) {
	std::string name = PickName(service.long_names);
	if(name.empty())
		name = PickName(service.medium_names);
	if(name.empty())
		name = PickName(service.short_names);
	if(name.empty())
		name = "(unnamed service)";

	// every widget below is Gtk::manage()'d: FlowBox::insert() wraps its child in an internal
	// FlowBoxChild, so ClearContent() can't just delete what get_children() hands back the way a
	// plain Gtk::Box's children could - letting GTK own and destroy all of this on remove() avoids
	// having to know/rely on that wrapping behaviour
	Gtk::Box *card = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
	card->get_style_context()->add_class("spi-card");
	card->set_size_request(280, -1);

	Gtk::Label *name_label = Gtk::manage(new Gtk::Label(name));
	name_label->get_style_context()->add_class("spi-name");
	name_label->set_halign(Gtk::ALIGN_START);
	name_label->set_line_wrap(true);
	card->pack_start(*name_label, false, false);

	Gtk::Label *bearer_label = Gtk::manage(new Gtk::Label(FormatDABBearer(service.has_dab_bearer, service.ecc, service.eid, service.sid, service.sid_32bit, service.scids)));
	bearer_label->get_style_context()->add_class("spi-bearer");
	bearer_label->set_halign(Gtk::ALIGN_START);
	bearer_label->set_selectable(true);
	card->pack_start(*bearer_label, false, false);

	if(!service.logos.empty()) {
		// a plain fixed-column Gtk::Grid rather than a (nested) Gtk::FlowBox: FlowBox's
		// height-for-width negotiation is unreliable once nested inside another container with a
		// constrained width (a long-standing GTK3 limitation) and was under-reporting this row's
		// height, making the logos stick out past the card's own bottom edge
		Gtk::Grid *logos_grid = Gtk::manage(new Gtk::Grid());
		logos_grid->set_row_spacing(6);
		logos_grid->set_column_spacing(6);
		logos_grid->set_margin_top(4);

		int i = 0;
		for(const SPI_LOGO& logo : service.logos) {
			logos_grid->attach(*BuildLogoBox(logo, logo_data), i % logos_per_row, i / logos_per_row, 1, 1);
			i++;
		}

		card->pack_start(*logos_grid, false, false);
	}

	return card;
}

Gtk::Widget *EDIScopeGTKSPIWindow::BuildGroupCard(const SPI_PROGRAMME_GROUP& group, const std::map<std::string,std::vector<uint8_t>>& logo_data) {
	std::string name = PickName(group.long_names);
	if(name.empty())
		name = PickName(group.medium_names);
	if(name.empty())
		name = PickName(group.short_names);
	if(name.empty())
		name = group.id.empty() ? "(unnamed group)" : group.id;

	static const std::map<int,std::string> type_names = {
		{SPI_PROGRAMME_GROUP::TYPE_SERIES, "series"},
		{SPI_PROGRAMME_GROUP::TYPE_SHOW, "show"},
		{SPI_PROGRAMME_GROUP::TYPE_PROGRAM_CONCEPT, "programme concept"},
		{SPI_PROGRAMME_GROUP::TYPE_MAGAZINE, "magazine"},
		{SPI_PROGRAMME_GROUP::TYPE_PROGRAM_COMPILATION, "programme compilation"},
		{SPI_PROGRAMME_GROUP::TYPE_OTHER_COLLECTION, "collection"},
		{SPI_PROGRAMME_GROUP::TYPE_OTHER_CHOICE, "choice"},
		{SPI_PROGRAMME_GROUP::TYPE_TOPIC, "topic"},
	};

	Gtk::Box *card = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 6));
	card->get_style_context()->add_class("spi-card");
	card->set_size_request(280, -1);

	Gtk::Label *name_label = Gtk::manage(new Gtk::Label(name));
	name_label->get_style_context()->add_class("spi-name");
	name_label->set_halign(Gtk::ALIGN_START);
	name_label->set_line_wrap(true);
	card->pack_start(*name_label, false, false);

	std::string subtitle;
	std::map<int,std::string>::const_iterator type_it = type_names.find(group.type);
	if(type_it != type_names.end())
		subtitle = type_it->second;
	if(group.num_of_items >= 0)
		subtitle += (subtitle.empty() ? "" : ", ") + std::to_string(group.num_of_items) + " items";
	if(!subtitle.empty()) {
		Gtk::Label *subtitle_label = Gtk::manage(new Gtk::Label(subtitle));
		subtitle_label->get_style_context()->add_class("spi-group-type");
		subtitle_label->set_halign(Gtk::ALIGN_START);
		card->pack_start(*subtitle_label, false, false);
	}

	if(!group.logos.empty()) {
		// see BuildServiceCard() for why a Grid is used here instead of a (nested) FlowBox
		Gtk::Grid *logos_grid = Gtk::manage(new Gtk::Grid());
		logos_grid->set_row_spacing(6);
		logos_grid->set_column_spacing(6);
		logos_grid->set_margin_top(4);

		int i = 0;
		for(const SPI_LOGO& logo : group.logos) {
			logos_grid->attach(*BuildLogoBox(logo, logo_data), i % logos_per_row, i / logos_per_row, 1, 1);
			i++;
		}

		card->pack_start(*logos_grid, false, false);
	}

	return card;
}

void EDIScopeGTKSPIWindow::ClearContent() {
	for(Gtk::Widget *child : content_flow.get_children())
		content_flow.remove(*child);
	for(Gtk::Widget *child : groupes_flow.get_children())
		groupes_flow.remove(*child);
	for(Gtk::Widget *child : programme_timeline.get_children())
		programme_timeline.remove(*child);
}

void EDIScopeGTKSPIWindow::RebuildProgrammeSelector() {
	std::string prev_selection = programme_service_selector.get_active_text();

	programme_service_selector.remove_all();
	int select_index = -1, i = 0;
	for(const std::pair<const std::pair<uint32_t,std::string>,SPI_SCHEDULE>& kv : last_schedules) {
		const std::string& date_key = kv.first.second;
		std::string label = FindServiceName(kv.first.first) + (date_key.empty() ? "" : " - " + date_key);
		programme_service_selector.append(label);
		if(label == prev_selection)
			select_index = i;
		i++;
	}

	if(!last_schedules.empty())
		programme_service_selector.set_active(select_index >= 0 ? select_index : 0);
}

void EDIScopeGTKSPIWindow::RedrawProgrammeTimeline() {
	for(Gtk::Widget *child : programme_timeline.get_children())
		programme_timeline.remove(*child);

	if(last_schedules.empty()) {
		programme_scroll.hide();
		programme_empty_label.show();
		return;
	}

	int selected = programme_service_selector.get_active_row_number();
	if(selected < 0)
		return;

	std::map<std::pair<uint32_t,std::string>,SPI_SCHEDULE>::const_iterator it = last_schedules.begin();
	std::advance(it, selected);
	const SPI_SCHEDULE& schedule = it->second;

	programme_empty_label.hide();
	programme_scroll.show();

	static const int ruler_width = 50;
	static const int block_width = 480;
	static const int minutes_per_day = 24 * 60;

	programme_timeline.set_size_request(ruler_width + block_width + 20, (int) (minutes_per_day * programme_px_per_min) + 20);

	for(int h = 0; h < 24; h++) {
		int y = (int) (h * 60 * programme_px_per_min);

		char buf[6];
		snprintf(buf, sizeof(buf), "%02d:00", h);
		Gtk::Label *hour_label = Gtk::manage(new Gtk::Label(buf));
		hour_label->get_style_context()->add_class("spi-hour-label");
		programme_timeline.put(*hour_label, 0, y - 6);

		Gtk::Box *hour_line = Gtk::manage(new Gtk::Box());
		hour_line->get_style_context()->add_class("spi-hour-line");
		hour_line->set_size_request(block_width, 1);
		programme_timeline.put(*hour_line, ruler_width, y);
	}

	for(const SPI_PROGRAMME& prog : schedule.programmes) {
		if(!prog.time.valid)
			continue;

		// apply LTO the same way FICDecoder::ConvertDateTimeToString does, to position the block
		// at the same local time shown in its label
		struct tm local_tm = prog.time.utc.dt;
		if(prog.time.lto != FIC_ENSEMBLE::lto_none)
			local_tm.tm_min += prog.time.lto * 30;
		local_tm.tm_isdst = -1;
		if(mktime(&local_tm) == (time_t) -1)
			continue;

		int start_min = local_tm.tm_hour * 60 + local_tm.tm_min;
		int duration_min = prog.time.duration_seconds >= 0 ? prog.time.duration_seconds / 60 : 15;

		int y = (int) (start_min * programme_px_per_min);
		int height = std::max(20, (int) (duration_min * programme_px_per_min));

		std::string name = PickName(prog.long_names);
		if(name.empty())
			name = PickName(prog.medium_names);
		if(name.empty())
			name = PickName(prog.short_names);
		if(name.empty())
			name = "(unnamed programme)";

		char time_buf[12];
		snprintf(time_buf, sizeof(time_buf), "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);

		Gtk::Box *block = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 1));
		block->get_style_context()->add_class("spi-programme-block");
		block->set_size_request(block_width, height);

		Gtk::Label *name_label = Gtk::manage(new Gtk::Label(name));
		name_label->get_style_context()->add_class("spi-programme-name");
		name_label->set_halign(Gtk::ALIGN_START);
		name_label->set_line_wrap(true);
		block->pack_start(*name_label, false, false);

		Gtk::Label *time_label = Gtk::manage(new Gtk::Label(std::string(time_buf) + (prog.time.duration_seconds >= 0 ? " (" + std::to_string(duration_min) + " min)" : "")));
		time_label->get_style_context()->add_class("spi-programme-time");
		time_label->set_halign(Gtk::ALIGN_START);
		block->pack_start(*time_label, false, false);

		programme_timeline.put(*block, ruler_width + 10, y);
	}

	programme_timeline.show_all();
}

void EDIScopeGTKSPIWindow::on_programme_service_changed() {
	RedrawProgrammeTimeline();
}

void EDIScopeGTKSPIWindow::RedrawXML() {
	switch(xml_selector.get_active_row_number()) {
	case 0:
		xml_buffer->set_text(SPIDecoder::ToXML(last_si));
		break;
	case 1: {
		std::string xml;
		for(const std::pair<const std::pair<uint32_t,std::string>,SPI_SCHEDULE>& kv : last_schedules)
			xml += SPIDecoder::ToXML(kv.second) + "\n";
		xml_buffer->set_text(xml);
		break;
	}
	case 2:
		xml_buffer->set_text(SPIDecoder::ToXML(last_gi));
		break;
	}
}

void EDIScopeGTKSPIWindow::on_xml_selector_changed() {
	RedrawXML();
}

void EDIScopeGTKSPIWindow::Update(const SPI_SERVICE_INFORMATION& info, const std::map<std::string,std::vector<uint8_t>>& logo_data) {
	last_si = info;

	for(Gtk::Widget *child : content_flow.get_children())
		content_flow.remove(*child);

	if(info.services.empty()) {
		scroll.hide();
		empty_label.show();
	} else {
		empty_label.hide();
		scroll.show();

		for(const SPI_SERVICE& service : info.services) {
			Gtk::Widget *card = BuildServiceCard(service, logo_data);
			content_flow.insert(*card, -1);
			card->show_all();
		}
	}

	RebuildProgrammeSelector();	// service names shown there come from here
	if(xml_selector.get_active_row_number() == 0)
		RedrawXML();
}

void EDIScopeGTKSPIWindow::UpdateSchedule(const SPI_SCHEDULE& schedule) {
	if(!schedule.has_scope_service)
		return;

	std::string date_key = SPIDecoder::FormatDate(schedule.scope_start);
	last_schedules[std::make_pair(schedule.sid, date_key)] = schedule;
	RebuildProgrammeSelector();
	RedrawProgrammeTimeline();

	if(xml_selector.get_active_row_number() == 1)
		RedrawXML();
}

void EDIScopeGTKSPIWindow::UpdateGroupInformation(const SPI_GROUP_INFORMATION& info, const std::map<std::string,std::vector<uint8_t>>& logo_data) {
	last_gi = info;

	for(Gtk::Widget *child : groupes_flow.get_children())
		groupes_flow.remove(*child);

	if(info.groups.empty()) {
		groupes_scroll.hide();
		groupes_empty_label.show();
	} else {
		groupes_empty_label.hide();
		groupes_scroll.show();

		for(const SPI_PROGRAMME_GROUP& group : info.groups) {
			Gtk::Widget *card = BuildGroupCard(group, logo_data);
			groupes_flow.insert(*card, -1);
			card->show_all();
		}
	}

	if(xml_selector.get_active_row_number() == 2)
		RedrawXML();
}

void EDIScopeGTKSPIWindow::Clear() {
	ClearContent();
	last_si = SPI_SERVICE_INFORMATION();
	last_schedules.clear();
	last_gi = SPI_GROUP_INFORMATION();
	programme_service_selector.remove_all();

	scroll.hide();
	empty_label.show();
	programme_scroll.hide();
	programme_empty_label.show();
	groupes_scroll.hide();
	groupes_empty_label.show();
	xml_buffer->set_text("");
}
