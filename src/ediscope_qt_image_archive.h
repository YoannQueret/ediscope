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

#ifndef EDISCOPE_QT_IMAGE_ARCHIVE_H_
#define EDISCOPE_QT_IMAGE_ARCHIVE_H_

#include <map>
#include <set>
#include <string>
#include <vector>

#include <QString>

#include "mot_manager.h"


// --- EDIScopeQtImageArchive -----------------------------------------------------------------
// saves every image received - slides, SPI logos, RadioDNS logos - to a folder chosen in the
// settings, each kind enabled on its own: <folder>/eid-<EId>/sid-<SId>/{slideshow,spi,radioDNS}/.
// A slide is saved under its reception time ("2026-11-27 13-45-02 - <name>"), unless identical to
// the previous slide of the same service; a logo under its own name, and only when its content
// isn't saved there yet - a changed logo of the same name is kept next to it, with the time added
class EDIScopeQtImageArchive {
public:
	enum class Kind {SLIDESHOW, SPI, RDNS};

	struct SETTINGS {
		QString path;
		bool slideshow = false;
		bool spi = false;
		bool rdns = false;
	};
private:
	SETTINGS settings;
	std::map<uint32_t, size_t> last_slide_hash;	// SId -> content hash of its last slide
	std::set<QString> known;	// folder|name|content hash of every image already found or saved

	QString Folder(int eid, uint32_t sid, Kind kind) const;
	static QString FileName(const QString& name_or_url, const QString& extension);
	static QString ExtensionForMime(const QString& mime);
	static bool WriteFile(const QString& filename, const std::vector<uint8_t>& data);
public:
	// the settings saved by the settings window (QSettings "images/*"); path falls back to the
	// recordings folder
	static SETTINGS Load(const std::string& recordings_path);
	static void Save(const SETTINGS& settings);
	void SetSettings(const SETTINGS& settings) {this->settings = settings;}
	const SETTINGS& Settings() const {return settings;}
	bool Enabled(Kind kind) const;

	// a slide of the given service (the one played)
	void SaveSlide(int eid, uint32_t sid, const MOT_FILE& slide);
	// a logo (SPI or RadioDNS) of the given service; name_or_url: its MOT ContentName or URL
	void SaveLogo(Kind kind, int eid, uint32_t sid, const std::vector<uint8_t>& data, const std::string& name_or_url, const std::string& mime);

	// the extension of an image file, from its own content (PNG/JPEG/GIF signature), else from its
	// MIME type - e.g. ".png"; empty if neither tells
	static QString ImageExtension(const QByteArray& data, const QString& mime);

	// a new source: the next slide of a service is saved, even if identical to its last one
	void Reset() {last_slide_hash.clear();}
};

#endif /* EDISCOPE_QT_IMAGE_ARCHIVE_H_ */
