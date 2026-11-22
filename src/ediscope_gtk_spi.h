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

#ifndef DABLIN_GTK_SPI_H_
#define DABLIN_GTK_SPI_H_

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gtkmm.h>

#include "spi_decoder.h"


// --- EDIScopeGTKSPIWindow -----------------------------------------------------------------
// floating window for the SPI (ETSI TS 102 371/102 818) information decoded from the ensemble's
// data sub-channel, if any, as tabs mirroring SPI's three document types - "Services" (SI: a grid
// of cards, 4 per row, one per service: name, DAB bearer and logos), "Programme" (PI: a per-service
// day timeline/calendar) and "Groupes" (GI: a grid of cards, one per programme group) - plus "XML"
// (any of the three, reconstructed as readable XML, picked via a dropdown)
class EDIScopeGTKSPIWindow : public Gtk::Window {
private:
	Gtk::Notebook notebook;

	// "Services" tab (SI)
	Gtk::Box logos_page;
	Gtk::ScrolledWindow scroll;
	Gtk::FlowBox content_flow;
	Gtk::Label empty_label;

	// "Programme" tab (PI) - a single-column day timeline/calendar for one service at a time
	Gtk::Box programme_page;
	Gtk::ComboBoxText programme_service_selector;
	Gtk::ScrolledWindow programme_scroll;
	Gtk::Fixed programme_timeline;
	Gtk::Label programme_empty_label;

	// "Groupes" tab (GI)
	Gtk::Box groupes_page;
	Gtk::ScrolledWindow groupes_scroll;
	Gtk::FlowBox groupes_flow;
	Gtk::Label groupes_empty_label;

	// "XML" tab
	Gtk::Box xml_page;
	Gtk::ComboBoxText xml_selector;
	Gtk::ScrolledWindow xml_scroll;
	Gtk::TextView xml_textview;
	Glib::RefPtr<Gtk::TextBuffer> xml_buffer;

	Glib::RefPtr<Gtk::CssProvider> css_provider;

	// last decoded data of each SPI document type, kept around so the XML tab's selector and the
	// Programme tab's service selector can be (re)populated/redrawn independently of which
	// document last triggered an Update*() call
	SPI_SERVICE_INFORMATION last_si;
	// keyed by (service SId, "YYYY-MM-DD") - a PI object is per service per day, so a service can
	// have several of these at once
	std::map<std::pair<uint32_t,std::string>,SPI_SCHEDULE> last_schedules;
	SPI_GROUP_INFORMATION last_gi;

	// SId -> best-known FIC label, used by FindServiceName() as a fallback when SPI's own SI
	// doesn't describe that service
	std::map<uint32_t,std::string> known_service_names;

	bool positioned;

	static const int logos_per_row = 4;
	static const int logos_rows_on_open = 3;
	static constexpr double programme_px_per_min = 1.5;

	static std::string PickName(const std::map<std::string,std::string>& names);
	static std::string FormatDABBearer(bool has_dab_bearer, int ecc, int eid, uint32_t sid, bool sid_32bit, int scids);
	static std::string FormatLogoCaption(const SPI_LOGO& logo);
	std::string FindServiceName(uint32_t sid) const;

	Glib::RefPtr<Gdk::Pixbuf> LoadLogoPixbuf(const SPI_LOGO& logo, const std::map<std::string,std::vector<uint8_t>>& logo_data);
	static Glib::RefPtr<Gdk::Pixbuf> ScaleToThumbnail(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf);
	static bool OnLogoQueryTooltip(int x, int y, bool keyboard_tooltip, const Glib::RefPtr<Gtk::Tooltip>& tooltip, Glib::RefPtr<Gdk::Pixbuf> full_pixbuf);
	Gtk::Widget *BuildLogoBox(const SPI_LOGO& logo, const std::map<std::string,std::vector<uint8_t>>& logo_data);
	Gtk::Widget *BuildServiceCard(const SPI_SERVICE& service, const std::map<std::string,std::vector<uint8_t>>& logo_data);
	Gtk::Widget *BuildGroupCard(const SPI_PROGRAMME_GROUP& group, const std::map<std::string,std::vector<uint8_t>>& logo_data);

	void ClearContent();
	void RebuildProgrammeSelector();
	void RedrawProgrammeTimeline();
	void RedrawXML();
	void on_programme_service_changed();
	void on_xml_selector_changed();
public:
	EDIScopeGTKSPIWindow();
	~EDIScopeGTKSPIWindow();

	void AlignToParent();

	void Update(const SPI_SERVICE_INFORMATION& info, const std::map<std::string,std::vector<uint8_t>>& logo_data);
	void UpdateSchedule(const SPI_SCHEDULE& schedule);
	void UpdateGroupInformation(const SPI_GROUP_INFORMATION& info, const std::map<std::string,std::vector<uint8_t>>& logo_data);
	void SetKnownServiceNames(const std::map<uint32_t,std::string>& names);
	void Clear();
};

#endif /* DABLIN_GTK_SPI_H_ */
