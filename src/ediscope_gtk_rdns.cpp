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

#include "ediscope_gtk_rdns.h"

#include <set>

#include "tools.h"


static const char *RDNS_CSS = R"CSS(
.rdns-empty {
	color: #9299a5;
	font-size: 13px;
}
.rdns-title {
	font-weight: 700;
	font-size: 14px;
	color: #20242c;
}
.rdns-stage-label {
	font-weight: 600;
	font-size: 11px;
	color: #8d94a0;
}
.rdns-stage-ok {
	color: #2a8f3f;
}
.rdns-stage-fail {
	color: #c0392b;
}
.rdns-xml-view {
	font-family: monospace;
	font-size: 11px;
}
.rdns-logo-box {
	background-color: #f6f7f9;
	border: 1px solid #e7e9ec;
	border-radius: 6px;
	padding: 6px;
}
.rdns-logo-caption {
	font-size: 10px;
	color: #9299a5;
}
)CSS";


// --- EDIScopeGTKRDNSWindow -----------------------------------------------------------------
EDIScopeGTKRDNSWindow::EDIScopeGTKRDNSWindow() :
		main_box(Gtk::ORIENTATION_VERTICAL),
		toolbar(Gtk::ORIENTATION_HORIZONTAL),
		detail_box(Gtk::ORIENTATION_VERTICAL, 10),
		detail_header_box(Gtk::ORIENTATION_HORIZONTAL),
		positioned(false)
{
	set_title("rDNS");
	set_type_hint(Gdk::WINDOW_TYPE_HINT_UTILITY);
	set_default_size(760, 480);
	// deletable: closing via the window manager's own close button is handled (kept alive, just
	// hidden) by EDIScopeGTK, which also owns the toggle button that must stay in sync with it

	css_provider = Gtk::CssProvider::create();
	css_provider->load_from_data(RDNS_CSS);
	Gtk::StyleContext::add_provider_for_screen(Gdk::Screen::get_default(), css_provider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	test_all_button.set_label("Tester tous les services");
	test_all_button.signal_clicked().connect(sigc::mem_fun(*this, &EDIScopeGTKRDNSWindow::on_test_all_clicked));
	test_all_button.set_margin_start(6);
	test_all_button.set_margin_top(6);
	test_all_button.set_margin_bottom(6);
	toolbar.pack_start(test_all_button, false, false);

	empty_label.set_text("No audio service known yet on this ensemble.");
	empty_label.get_style_context()->add_class("rdns-empty");
	empty_label.set_margin_top(24);
	empty_label.set_line_wrap(true);

	list_store = Gtk::ListStore::create(list_cols);
	services_list.set_model(list_store);
	services_list.append_column("Service", list_cols.col_label);
	{
		Gtk::CellRendererText *status_renderer = Gtk::manage(new Gtk::CellRendererText());
		int col_index = services_list.append_column("Status", *status_renderer) - 1;
		Gtk::TreeViewColumn *status_col = services_list.get_column(col_index);
		// a plain add_attribute() binding would hand GTK an empty string whenever
		// col_status_color is "" (not tested / no color) and have it try to parse that as a
		// color ("Gtk-WARNING: Don't know color ''") - a cell_data_func can leave the property
		// unset instead, same pattern as EDIScopeGTK::RenderServiceDetailCell()'s own "R" column
		status_col->set_cell_data_func(*status_renderer, sigc::mem_fun(*this, &EDIScopeGTKRDNSWindow::RenderStatusCell));
	}
	services_list.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &EDIScopeGTKRDNSWindow::on_services_list_selection_changed));

	list_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
	list_scroll.add(services_list);
	list_scroll.set_size_request(280, -1);

	// --- detail pane (right side) ---
	detail_placeholder.set_text("Select a service on the left to see its RadioDNS resolution trace.");
	detail_placeholder.get_style_context()->add_class("rdns-empty");
	detail_placeholder.set_margin_top(24);
	detail_placeholder.set_line_wrap(true);
	detail_placeholder.set_valign(Gtk::ALIGN_START);

	detail_title.get_style_context()->add_class("rdns-title");
	detail_title.set_halign(Gtk::ALIGN_START);
	detail_title.set_line_wrap(true);
	detail_header_box.pack_start(detail_title, true, true);

	test_selected_button.set_label("Tester ce service");
	test_selected_button.signal_clicked().connect(sigc::mem_fun(*this, &EDIScopeGTKRDNSWindow::on_test_selected_clicked));
	detail_header_box.pack_start(test_selected_button, false, false);

	detail_grid.set_row_spacing(4);
	detail_grid.set_column_spacing(10);

	static const char *stage_titles[] = {"FQDN:", "GCC:", "DNS CNAME:", "DNS SRV (_radioepg._tcp):", "HTTP (SI.xml):"};
	Gtk::Label *stage_values[] = {&detail_fqdn_value, &detail_gcc_value, &detail_cname_value, &detail_srv_value, &detail_http_value};
	for(int i = 0; i < 5; i++) {
		Gtk::Label *title_label = Gtk::manage(new Gtk::Label(stage_titles[i]));
		title_label->get_style_context()->add_class("rdns-stage-label");
		title_label->set_halign(Gtk::ALIGN_START);
		title_label->set_valign(Gtk::ALIGN_START);
		detail_grid.attach(*title_label, 0, i, 1, 1);

		stage_values[i]->set_halign(Gtk::ALIGN_START);
		stage_values[i]->set_line_wrap(true);
		stage_values[i]->set_selectable(true);
		detail_grid.attach(*stage_values[i], 1, i, 1, 1);
	}

	// --- "Info" tab ---
	info_box.set_orientation(Gtk::ORIENTATION_VERTICAL);
	info_box.set_spacing(4);
	info_box.set_margin_start(10);
	info_box.set_margin_end(10);
	info_box.set_margin_top(10);
	info_box.set_margin_bottom(10);

	info_placeholder.set_text("Nothing decoded from the SI.xml for this service yet.");
	info_placeholder.get_style_context()->add_class("rdns-empty");
	info_placeholder.set_halign(Gtk::ALIGN_START);
	info_placeholder.set_line_wrap(true);

	info_provider_name.get_style_context()->add_class("rdns-title");
	info_provider_name.set_halign(Gtk::ALIGN_START);
	info_provider_name.set_line_wrap(true);
	info_provider_desc.set_halign(Gtk::ALIGN_START);
	info_provider_desc.set_line_wrap(true);
	info_provider_desc.set_selectable(true);

	info_provider_logos_flow.set_selection_mode(Gtk::SELECTION_NONE);
	info_provider_logos_flow.set_row_spacing(10);
	info_provider_logos_flow.set_column_spacing(10);
	info_provider_logos_flow.set_valign(Gtk::ALIGN_START);
	info_provider_logos_flow.set_margin_top(6);

	info_service_name.get_style_context()->add_class("rdns-title");
	info_service_name.set_halign(Gtk::ALIGN_START);
	info_service_name.set_line_wrap(true);
	info_service_name.set_margin_top(12);
	info_service_desc.set_halign(Gtk::ALIGN_START);
	info_service_desc.set_line_wrap(true);
	info_service_desc.set_selectable(true);

	info_box.pack_start(info_placeholder, false, false);
	info_box.pack_start(info_provider_name, false, false);
	info_box.pack_start(info_provider_desc, false, false);
	info_box.pack_start(info_provider_logos_flow, false, true);	// fill=true - without it, the FlowBox only gets its minimum width (one logo wide) and stacks vertically instead of flowing horizontally
	info_box.pack_start(info_service_name, false, false);
	info_box.pack_start(info_service_desc, false, false);

	info_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
	info_scroll.add(info_box);

	// --- "Logos" tab ---
	logos_empty_label.set_text("No logo found for this service in the SI.xml.");
	logos_empty_label.get_style_context()->add_class("rdns-empty");
	logos_empty_label.set_margin_top(24);
	logos_empty_label.set_line_wrap(true);

	logos_flow.set_selection_mode(Gtk::SELECTION_NONE);
	logos_flow.set_row_spacing(10);
	logos_flow.set_column_spacing(10);
	logos_flow.set_margin_start(10);
	logos_flow.set_margin_end(10);
	logos_flow.set_margin_top(10);
	logos_flow.set_margin_bottom(10);
	logos_flow.set_valign(Gtk::ALIGN_START);

	logos_scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
	logos_scroll.add(logos_flow);

	// --- "Bearers" tab ---
	bearers_store = Gtk::ListStore::create(bearers_cols);
	bearers_view.set_model(bearers_store);
	bearers_view.append_column("Bearer", bearers_cols.col_id);
	bearers_view.append_column("Bitrate", bearers_cols.col_bitrate);
	bearers_view.append_column("Cost", bearers_cols.col_cost);
	bearers_view.append_column("Type", bearers_cols.col_mime);
	for(int i = 0; i < 4; i++) {
		Gtk::TreeViewColumn *col = bearers_view.get_column(i);
		col->set_resizable(true);
		// orderable by clicking the header, same as the main window's "Services details" table
		bearers_store->set_sort_func(i, sigc::bind<0>(sigc::mem_fun(*this, &EDIScopeGTKRDNSWindow::BearersTableColumnCompare), i));
		col->set_sort_column(i);
	}
	bearers_view.add_events(Gdk::BUTTON_RELEASE_MASK);
	// button-RELEASE, not press: TreeView's own internal click handling was observed to consume
	// the first button-press-event of any click sequence entirely - our connected handler was
	// never even invoked for it, only for the second press of what GDK then reports as a
	// double-click. Release events aren't swallowed the same way
	bearers_view.signal_button_release_event().connect([this](GdkEventButton *button_event) -> bool {
		return on_bearers_view_button_release_event(button_event);
	});

	bearer_open_menu_item.set_label("Open");
	bearer_open_menu_item.signal_activate().connect(sigc::mem_fun(*this, &EDIScopeGTKRDNSWindow::on_bearer_open_clicked));
	bearer_context_menu.append(bearer_open_menu_item);

	bearer_copy_menu_item.set_label("Copy");
	bearer_copy_menu_item.signal_activate().connect(sigc::mem_fun(*this, &EDIScopeGTKRDNSWindow::on_bearer_copy_clicked));
	bearer_context_menu.append(bearer_copy_menu_item);

	bearer_context_menu.show_all();

	bearers_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	bearers_scroll.add(bearers_view);

	// --- "SI.xml" tab ---
	xml_textview.get_style_context()->add_class("rdns-xml-view");
	xml_textview.set_editable(false);
	xml_textview.set_monospace(true);
	xml_textview.set_left_margin(6);
	xml_textview.set_top_margin(6);
	xml_buffer = xml_textview.get_buffer();

	xml_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
	xml_scroll.add(xml_textview);

	result_notebook.append_page(info_scroll, "Info");
	result_notebook.append_page(logos_scroll, "Logos");
	result_notebook.append_page(bearers_scroll, "Bearers");
	result_notebook.append_page(xml_scroll, "SI.xml");
	result_notebook.set_vexpand(true);

	detail_box.pack_start(detail_header_box, false, false);
	detail_box.pack_start(detail_grid, false, false);
	detail_box.pack_start(result_notebook, true, true);
	detail_box.set_margin_start(10);
	detail_box.set_margin_end(10);
	detail_box.set_margin_top(10);
	detail_box.set_margin_bottom(10);

	paned.pack1(list_scroll, false, false);
	paned.pack2(detail_box, true, false);
	paned.set_position(280);

	main_box.pack_start(toolbar, false, false);
	main_box.pack_start(empty_label, false, false);
	main_box.pack_start(paned, true, true);
	add(main_box);

	show_all_children();
	empty_label.hide();
	detail_box.hide();
}

void EDIScopeGTKRDNSWindow::AlignToParent() {
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

Gtk::TreeModel::iterator EDIScopeGTKRDNSWindow::FindRow(int sid, int scids) {
	for(Gtk::TreeModel::iterator it = list_store->children().begin(); it != list_store->children().end(); ++it)
		if((int) (*it)[list_cols.col_sid] == sid && (int) (*it)[list_cols.col_scids] == scids)
			return it;
	return Gtk::TreeModel::iterator();
}

Glib::ustring EDIScopeGTKRDNSWindow::ShortStatus(const RDNS_RESULT& result, Glib::ustring& color) {
	if(result.FullyResolved()) {
		color = "#2a8f3f";
		return "OK";
	}
	if(result.http.attempted) {
		color = "#c0392b";
		return "Blocked: HTTP";
	}
	if(result.srv.attempted) {
		color = "#c0392b";
		return "Blocked: DNS SRV";
	}
	if(result.cname.attempted) {
		color = "#c0392b";
		return "Blocked: DNS CNAME";
	}
	color = "";
	return "";
}

void EDIScopeGTKRDNSWindow::RefreshRowStatus(const Gtk::TreeModel::iterator& row_it) {
	Gtk::TreeModel::Row row = *row_it;
	int sid = row[list_cols.col_sid];
	int scids = row[list_cols.col_scids];
	std::pair<int,int> key(sid, scids);

	std::map<std::pair<int,int>,bool>::const_iterator pending_it = pending.find(key);
	if(pending_it != pending.end() && pending_it->second) {
		row[list_cols.col_status] = "Testing…";
		row[list_cols.col_status_color] = "#8d94a0";
		return;
	}

	std::map<std::pair<int,int>,RDNS_RESULT>::const_iterator result_it = results.find(key);
	if(result_it == results.end()) {
		row[list_cols.col_status] = "";
		row[list_cols.col_status_color] = "";
		return;
	}

	Glib::ustring color;
	Glib::ustring status = ShortStatus(result_it->second, color);
	row[list_cols.col_status] = status;
	row[list_cols.col_status_color] = color;
}

void EDIScopeGTKRDNSWindow::RenderStatusCell(Gtk::CellRenderer* renderer, const Gtk::TreeModel::iterator& it) {
	Gtk::CellRendererText *text_renderer = static_cast<Gtk::CellRendererText*>(renderer);
	Glib::ustring color = (*it)[list_cols.col_status_color];

	text_renderer->property_text() = (Glib::ustring) (*it)[list_cols.col_status];
	text_renderer->property_foreground_set() = !color.empty();
	if(!color.empty())
		text_renderer->property_foreground() = color;
}

int EDIScopeGTKRDNSWindow::BearersTableColumnCompare(int column_id, const Gtk::TreeModel::iterator& a, const Gtk::TreeModel::iterator& b) {
	switch(column_id) {
	case 1: {	// Bitrate - numeric, not the formatted "128 kBit/s" text
		int va = (*a)[bearers_cols.col_bitrate_sort];
		int vb = (*b)[bearers_cols.col_bitrate_sort];
		return va - vb; }
	case 2:		// Cost
		return Glib::ustring((*a)[bearers_cols.col_cost]).compare((*b)[bearers_cols.col_cost]);
	case 3:		// Type
		return Glib::ustring((*a)[bearers_cols.col_mime]).compare((*b)[bearers_cols.col_mime]);
	default:	// Bearer
		return Glib::ustring((*a)[bearers_cols.col_id]).compare((*b)[bearers_cols.col_id]);
	}
}

void EDIScopeGTKRDNSWindow::ShowDetailFor(int sid, int scids) {
	Gtk::TreeModel::iterator row_it = FindRow(sid, scids);
	if(!row_it) {
		detail_box.hide();
		detail_placeholder.show();
		return;
	}

	detail_placeholder.hide();
	detail_box.show();

	Gtk::TreeModel::Row row = *row_it;
	Glib::ustring label = row[list_cols.col_label];
	detail_title.set_text(label + "  (SId " + StringTools::SidToHex(sid) + ", SCIdS " + std::to_string(scids) + ")");

	std::pair<int,int> key(sid, scids);
	std::map<std::pair<int,int>,bool>::const_iterator pending_it = pending.find(key);
	bool is_pending = pending_it != pending.end() && pending_it->second;
	test_selected_button.set_sensitive(!is_pending);

	std::map<std::pair<int,int>,RDNS_RESULT>::const_iterator result_it = results.find(key);
	if(result_it == results.end()) {
		detail_fqdn_value.set_text(is_pending ? "(testing…)" : "(not tested yet)");
		detail_gcc_value.set_text("");
		detail_cname_value.set_text("");
		detail_srv_value.set_text("");
		detail_http_value.set_text("");
		result_notebook.hide();
		return;
	}

	const RDNS_RESULT& result = result_it->second;
	detail_fqdn_value.set_text(result.fqdn.empty() ? "(couldn't be built)" : result.fqdn);
	detail_gcc_value.set_text(result.gcc.empty() ? "(unknown)" : result.gcc);

	Gtk::Label *stage_labels[] = {&detail_cname_value, &detail_srv_value, &detail_http_value};
	const RDNS_STAGE *stages[] = {&result.cname, &result.srv, &result.http};
	for(int i = 0; i < 3; i++) {
		const RDNS_STAGE& stage = *stages[i];
		std::string text = !stage.attempted ? "(not reached)" : (stage.success ? "\xE2\x9C\x93 " : "\xE2\x9C\x97 ") + stage.detail;
		stage_labels[i]->set_text(text);

		Glib::RefPtr<Gtk::StyleContext> ctx = stage_labels[i]->get_style_context();
		ctx->remove_class("rdns-stage-ok");
		ctx->remove_class("rdns-stage-fail");
		if(stage.attempted)
			ctx->add_class(stage.success ? "rdns-stage-ok" : "rdns-stage-fail");
	}

	if(result.FullyResolved()) {
		result_notebook.show();
		PopulateResultTabs(result);
	} else {
		result_notebook.hide();
	}
}

Glib::RefPtr<Gdk::Pixbuf> EDIScopeGTKRDNSWindow::ScaleToThumbnail(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf) {
	static const int max_thumb = 96;
	int w = pixbuf->get_width();
	int h = pixbuf->get_height();
	if(w <= max_thumb && h <= max_thumb)
		return pixbuf;

	double scale = (double) max_thumb / (w > h ? w : h);
	int new_w = std::max(1, (int) (w * scale));
	int new_h = std::max(1, (int) (h * scale));
	return pixbuf->scale_simple(new_w, new_h, Gdk::INTERP_BILINEAR);
}

bool EDIScopeGTKRDNSWindow::OnLogoQueryTooltip(int /*x*/, int /*y*/, bool /*keyboard_tooltip*/, const Glib::RefPtr<Gtk::Tooltip>& tooltip, Glib::RefPtr<Gdk::Pixbuf> full_pixbuf) {
	Gtk::Image *full_image = Gtk::manage(new Gtk::Image(full_pixbuf));
	tooltip->set_custom(*full_image);
	return true;
}

Gtk::Widget *EDIScopeGTKRDNSWindow::BuildLogoBox(const RDNS_XML_LOGO& logo) {
	Gtk::Box *box = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 2));
	box->get_style_context()->add_class("rdns-logo-box");

	Gtk::Image *image = Gtk::manage(new Gtk::Image());
	Glib::RefPtr<Gdk::Pixbuf> full_pixbuf;
	if(!logo.data.empty()) {
		try {
			Glib::RefPtr<Gdk::PixbufLoader> loader = Gdk::PixbufLoader::create();
			loader->write(&logo.data[0], logo.data.size());
			loader->close();
			full_pixbuf = loader->get_pixbuf();
		} catch(const Glib::Error&) {
			// full_pixbuf stays null - falls through to the "image-missing" case below
		}
	}

	if(full_pixbuf) {
		image->set(ScaleToThumbnail(full_pixbuf));
		// shows the logo at its real, undownscaled size on hover - same as the SPI window's own
		// logos (see EDIScopeGTKSPIWindow::OnLogoQueryTooltip())
		box->set_has_tooltip(true);
		box->signal_query_tooltip().connect(sigc::bind(sigc::ptr_fun(&EDIScopeGTKRDNSWindow::OnLogoQueryTooltip), full_pixbuf));
	} else {
		image->set_from_icon_name("image-missing", Gtk::ICON_SIZE_DIALOG);
		image->set_tooltip_text(logo.url);
	}
	image->set_halign(Gtk::ALIGN_CENTER);

	std::string caption = logo.type;
	if(logo.width > 0 && logo.height > 0)
		caption += " " + std::to_string(logo.width) + "x" + std::to_string(logo.height);
	if(!logo.data.empty())
		caption += "\n" + StringTools::FormatByteSize(logo.data.size());
	Gtk::Label *caption_label = Gtk::manage(new Gtk::Label(caption));
	caption_label->get_style_context()->add_class("rdns-logo-caption");
	caption_label->set_justify(Gtk::JUSTIFY_CENTER);
	caption_label->set_halign(Gtk::ALIGN_CENTER);
	caption_label->set_line_wrap(true);
	caption_label->set_max_width_chars(16);

	box->pack_start(*image, false, false);
	box->pack_start(*caption_label, false, false);
	return box;
}

void EDIScopeGTKRDNSWindow::PopulateResultTabs(const RDNS_RESULT& result) {
	// --- Info ---
	bool have_provider_info = !result.si_provider.IsEmpty();
	bool have_service_info = result.si_service_matched && !result.si_service.IsEmpty();

	info_placeholder.set_visible(!result.si_parsed || (!have_provider_info && !have_service_info));

	info_provider_name.set_visible(have_provider_info);
	info_provider_desc.set_visible(have_provider_info && !result.si_provider.short_description.empty());
	if(have_provider_info) {
		std::string name = !result.si_provider.long_name.empty() ? result.si_provider.long_name :
				!result.si_provider.medium_name.empty() ? result.si_provider.medium_name : result.si_provider.short_name;
		info_provider_name.set_text("Service Provider: " + name);
		info_provider_desc.set_text(!result.si_provider.long_description.empty() ? result.si_provider.long_description : result.si_provider.short_description);
	}

	for(Gtk::Widget *child : info_provider_logos_flow.get_children())
		info_provider_logos_flow.remove(*child);
	info_provider_logos_flow.set_visible(!result.si_provider.logos.empty());
	for(const RDNS_XML_LOGO& logo : result.si_provider.logos) {
		Gtk::Widget *box = BuildLogoBox(logo);
		info_provider_logos_flow.insert(*box, -1);
		box->show_all();
	}

	info_service_name.set_visible(have_service_info);
	info_service_desc.set_visible(have_service_info && !result.si_service.short_description.empty());
	if(have_service_info) {
		std::string name = !result.si_service.long_name.empty() ? result.si_service.long_name :
				!result.si_service.medium_name.empty() ? result.si_service.medium_name : result.si_service.short_name;
		info_service_name.set_text("This service: " + name);
		info_service_desc.set_text(!result.si_service.long_description.empty() ? result.si_service.long_description : result.si_service.short_description);
	}

	// --- Logos --- (this service's own - the serviceProvider's are shown under "Service
	// Provider" in the Info tab instead, see info_provider_logos_flow above)
	for(Gtk::Widget *child : logos_flow.get_children())
		logos_flow.remove(*child);

	logos_empty_label.set_visible(result.si_service.logos.empty());
	logos_scroll.set_visible(!result.si_service.logos.empty());
	for(const RDNS_XML_LOGO& logo : result.si_service.logos) {
		Gtk::Widget *box = BuildLogoBox(logo);
		logos_flow.insert(*box, -1);
		box->show_all();
	}

	// --- Bearers ---
	bearers_store->clear();
	for(const RDNS_XML_BEARER& bearer : result.si_service.bearers) {
		Gtk::TreeModel::iterator row_it = bearers_store->append();
		Gtk::TreeModel::Row row = *row_it;
		row[bearers_cols.col_id] = bearer.id;
		row[bearers_cols.col_bitrate] = bearer.bitrate.empty() ? "" : bearer.bitrate + " kBit/s";
		row[bearers_cols.col_bitrate_sort] = bearer.bitrate.empty() ? -1 : atoi(bearer.bitrate.c_str());
		row[bearers_cols.col_cost] = bearer.cost;
		row[bearers_cols.col_mime] = bearer.mime;
	}

	// --- SI.xml ---
	xml_buffer->set_text(result.si_parsed ? result.si_xml : (result.si_xml.empty() ? "" : result.si_xml + "\n\n(couldn't be parsed as well-formed XML)"));
}

void EDIScopeGTKRDNSWindow::RequestTest(int sid, int scids, int ecc, int eid) {
	std::pair<int,int> key(sid, scids);
	if(pending[key])
		return;
	pending[key] = true;

	Gtk::TreeModel::iterator row_it = FindRow(sid, scids);
	if(row_it)
		RefreshRowStatus(row_it);

	Gtk::TreeModel::iterator selected_it = services_list.get_selection()->get_selected();
	if(selected_it && (int) (*selected_it)[list_cols.col_sid] == sid && (int) (*selected_it)[list_cols.col_scids] == scids)
		test_selected_button.set_sensitive(false);

	if(test_requested_slot)
		test_requested_slot(sid, scids, ecc, eid);
}

void EDIScopeGTKRDNSWindow::on_services_list_selection_changed() {
	Gtk::TreeModel::iterator it = services_list.get_selection()->get_selected();
	if(!it) {
		ShowDetailFor(-1, -1);
		return;
	}

	ShowDetailFor((*it)[list_cols.col_sid], (*it)[list_cols.col_scids]);
}

void EDIScopeGTKRDNSWindow::on_test_all_clicked() {
	for(const Gtk::TreeModel::Row& row : list_store->children())
		RequestTest(row[list_cols.col_sid], row[list_cols.col_scids], row[list_cols.col_ecc], row[list_cols.col_eid]);
}

void EDIScopeGTKRDNSWindow::on_test_selected_clicked() {
	Gtk::TreeModel::iterator it = services_list.get_selection()->get_selected();
	if(!it)
		return;

	RequestTest((*it)[list_cols.col_sid], (*it)[list_cols.col_scids], (*it)[list_cols.col_ecc], (*it)[list_cols.col_eid]);
}

void EDIScopeGTKRDNSWindow::SetServices(const std::vector<SERVICE_ENTRY>& services) {
	int prev_sid = -1, prev_scids = -1;
	Gtk::TreeModel::iterator prev_it = services_list.get_selection()->get_selected();
	if(prev_it) {
		prev_sid = (*prev_it)[list_cols.col_sid];
		prev_scids = (*prev_it)[list_cols.col_scids];
	}

	list_store->clear();

	std::set<std::pair<int,int>> current_keys;
	for(const SERVICE_ENTRY& s : services)
		current_keys.insert(std::make_pair(s.sid, s.scids));

	// drop stale results/pending state for services no longer listed (e.g. a hot mux reconfiguration)
	for(std::map<std::pair<int,int>,RDNS_RESULT>::iterator it = results.begin(); it != results.end();) {
		if(current_keys.find(it->first) == current_keys.end())
			it = results.erase(it);
		else
			++it;
	}
	for(std::map<std::pair<int,int>,bool>::iterator it = pending.begin(); it != pending.end();) {
		if(current_keys.find(it->first) == current_keys.end())
			it = pending.erase(it);
		else
			++it;
	}

	Gtk::TreeModel::iterator restore_it;
	for(const SERVICE_ENTRY& s : services) {
		Gtk::TreeModel::iterator row_it = list_store->append();
		Gtk::TreeModel::Row row = *row_it;
		row[list_cols.col_sid] = s.sid;
		row[list_cols.col_scids] = s.scids;
		row[list_cols.col_ecc] = s.ecc;
		row[list_cols.col_eid] = s.eid;
		row[list_cols.col_label] = s.label;
		RefreshRowStatus(row_it);

		if(s.sid == prev_sid && s.scids == prev_scids)
			restore_it = row_it;
	}

	empty_label.set_visible(services.empty());
	list_scroll.set_visible(!services.empty());

	if(restore_it) {
		services_list.get_selection()->select(restore_it);
	} else if(!services.empty()) {
		// nothing was selected before (e.g. the window just opened) - select the first row rather
		// than requiring an extra click before its trace can be seen
		services_list.get_selection()->select(list_store->children().begin());
	} else {
		ShowDetailFor(-1, -1);
	}
}

void EDIScopeGTKRDNSWindow::UpdateResult(int sid, int scids, const RDNS_RESULT& result) {
	std::pair<int,int> key(sid, scids);
	results[key] = result;
	pending[key] = false;

	Gtk::TreeModel::iterator row_it = FindRow(sid, scids);
	if(row_it)
		RefreshRowStatus(row_it);

	Gtk::TreeModel::iterator selected_it = services_list.get_selection()->get_selected();
	if(selected_it && (int) (*selected_it)[list_cols.col_sid] == sid && (int) (*selected_it)[list_cols.col_scids] == scids)
		ShowDetailFor(sid, scids);
}

void EDIScopeGTKRDNSWindow::Clear() {
	list_store->clear();
	results.clear();
	pending.clear();
	empty_label.show();
	list_scroll.hide();
	ShowDetailFor(-1, -1);
}

bool EDIScopeGTKRDNSWindow::IsStreamBearer(const std::string& id) {
	// "dab:..."/"fm:..." (ETSI TS 103 270 bearerURIs) identify a broadcast bearer, not an address
	// of anything this app could open or usefully copy as a link - an actual stream is the only
	// kind of bearer id that is itself a fetchable URL
	return id.compare(0, 7, "http://") == 0 || id.compare(0, 8, "https://") == 0;
}

bool EDIScopeGTKRDNSWindow::on_bearers_view_button_release_event(GdkEventButton *button_event) {
	if(button_event->type != GDK_BUTTON_RELEASE || button_event->button != 3)
		return false;

	Gtk::TreeModel::Path path;
	if(!bearers_view.get_path_at_pos((int) button_event->x, (int) button_event->y, path))
		return false;

	Gtk::TreeModel::iterator row_it = bearers_store->get_iter(path);
	if(!row_it)
		return false;

	Glib::ustring id = (*row_it)[bearers_cols.col_id];
	if(!IsStreamBearer(id))
		return false;

	context_bearer_url = id;
	bearers_view.set_cursor(path);

	// deferred to idle rather than popped up synchronously right here - avoids the menu's own
	// pointer grab racing against whatever GTK is still doing to finish handling this same click
	guint button = button_event->button;
	guint32 event_time = button_event->time;
	Glib::signal_idle().connect_once([this, button, event_time]() {
		bearer_context_menu.popup(button, event_time);
	});
	return true;
}

void EDIScopeGTKRDNSWindow::on_bearer_open_clicked() {
	if(context_bearer_url.empty())
		return;

	show_uri(context_bearer_url, GDK_CURRENT_TIME);
}

void EDIScopeGTKRDNSWindow::on_bearer_copy_clicked() {
	if(context_bearer_url.empty())
		return;

	Gtk::Clipboard::get()->set_text(context_bearer_url);
}
