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

#include "ediscope_qt_image_archive.h"

#include <cstdio>
#include <functional>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include "tools.h"


static size_t Hash(const std::vector<uint8_t>& data) {
	return std::hash<std::string>()(std::string(data.begin(), data.end()));
}

static QString Timestamp() {
	return QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm-ss");
}


// --- EDIScopeQtImageArchive -----------------------------------------------------------------
EDIScopeQtImageArchive::SETTINGS EDIScopeQtImageArchive::Load(const std::string& recordings_path) {
	QSettings qs;
	SETTINGS s;
	s.path = qs.value("images/path").toString();
	if(s.path.isEmpty())
		s.path = QString::fromStdString(recordings_path);
	s.slideshow = qs.value("images/slideshow", false).toBool();
	s.spi = qs.value("images/spi", false).toBool();
	s.rdns = qs.value("images/rdns", false).toBool();
	return s;
}

void EDIScopeQtImageArchive::Save(const SETTINGS& s) {
	QSettings qs;
	qs.setValue("images/path", s.path);
	qs.setValue("images/slideshow", s.slideshow);
	qs.setValue("images/spi", s.spi);
	qs.setValue("images/rdns", s.rdns);
}

bool EDIScopeQtImageArchive::Enabled(Kind kind) const {
	if(settings.path.isEmpty())
		return false;
	switch(kind) {
	case Kind::SLIDESHOW:	return settings.slideshow;
	case Kind::SPI:			return settings.spi;
	default:				return settings.rdns;
	}
}

QString EDIScopeQtImageArchive::Folder(int eid, uint32_t sid, Kind kind) const {
	QString kind_name = kind == Kind::SLIDESHOW ? "slideshow" : (kind == Kind::SPI ? "spi" : "radioDNS");
	return settings.path + "/eid-" + QString::fromStdString(StringTools::IntToHex(eid, 4).substr(2)) +
			"/sid-" + QString::fromStdString(StringTools::SidToHex((int) sid)) + "/" + kind_name;
}

QString EDIScopeQtImageArchive::ImageExtension(const QByteArray& data, const QString& mime) {
	// the content first: a logo's MIME type is often missing (or wrong)
	if(data.startsWith("\x89PNG"))
		return ".png";
	if(data.startsWith("\xFF\xD8\xFF"))
		return ".jpg";
	if(data.startsWith("GIF8"))
		return ".gif";
	return ExtensionForMime(mime);
}

QString EDIScopeQtImageArchive::ExtensionForMime(const QString& mime) {
	if(mime.contains("png"))
		return ".png";
	if(mime.contains("jpeg") || mime.contains("jpg"))
		return ".jpg";
	if(mime.contains("gif"))
		return ".gif";
	if(mime.contains("svg"))
		return ".svg";
	return QString();
}

QString EDIScopeQtImageArchive::FileName(const QString& name_or_url, const QString& extension) {
	// the last part of a path or URL, without a query, and without characters a file name can't have
	QString name = name_or_url.section('?', 0, 0).section('/', -1);
	for(QChar c : QString("\\:*?\"<>|"))
		name.replace(c, '_');
	if(!name.contains('.'))
		name += extension;
	return name;
}

bool EDIScopeQtImageArchive::WriteFile(const QString& filename, const std::vector<uint8_t>& data) {
	QFile file(filename);
	if(!file.open(QIODevice::WriteOnly) || file.write((const char*) data.data(), (qint64) data.size()) != (qint64) data.size()) {
		fprintf(stderr, "EDIScopeQtImageArchive: error while writing '%s': %s\n", filename.toUtf8().constData(), file.errorString().toUtf8().constData());
		return false;
	}
	return true;
}

void EDIScopeQtImageArchive::SaveSlide(int eid, uint32_t sid, const MOT_FILE& slide) {
	if(!Enabled(Kind::SLIDESHOW) || eid < 0 || slide.data.empty() || slide.content_type != MOT_FILE::CONTENT_TYPE_IMAGE)
		return;

	// the same slide again (e.g. repeated in the carousel): kept once
	size_t hash = Hash(slide.data);
	std::map<uint32_t, size_t>::iterator it = last_slide_hash.find(sid);
	if(it != last_slide_hash.end() && it->second == hash)
		return;
	last_slide_hash[sid] = hash;

	QString extension = slide.content_sub_type == MOT_FILE::CONTENT_SUB_TYPE_PNG ? ".png" : ".jpg";
	QString name = FileName(QString::fromStdString(slide.content_name), extension);
	if(name.isEmpty() || name == extension)
		name = "slide" + extension;

	QString folder = Folder(eid, sid, Kind::SLIDESHOW);
	QDir().mkpath(folder);
	QString filename = folder + "/" + Timestamp() + " - " + name;
	if(!QFileInfo::exists(filename))	// the same name, within the same second: kept once
		WriteFile(filename, slide.data);
}

void EDIScopeQtImageArchive::SaveLogo(Kind kind, int eid, uint32_t sid, const std::vector<uint8_t>& data, const std::string& name_or_url, const std::string& mime) {
	if(!Enabled(kind) || eid < 0 || data.empty())
		return;

	QString folder = Folder(eid, sid, kind);
	QString extension = ImageExtension(QByteArray((const char*) data.data(), (int) data.size()), QString::fromStdString(mime));
	QString name = FileName(QString::fromStdString(name_or_url), extension);
	if(name.isEmpty())
		name = "logo" + extension;

	QString key = folder + "|" + name + "|" + QString::number(Hash(data));
	if(known.count(key))
		return;
	known.insert(key);

	// already saved - under its own name, or as a timestamped version of it ("<name> (<time>).<ext>")
	QFileInfo name_info(name);
	QString stem = name_info.completeBaseName();
	QString suffix = name_info.suffix().isEmpty() ? QString() : "." + name_info.suffix();
	QDir dir(folder);
	QByteArray content((const char*) data.data(), (int) data.size());
	for(const QFileInfo& existing : dir.entryInfoList({stem + suffix, stem + " (*)" + suffix}, QDir::Files)) {
		if(existing.size() != (qint64) data.size())
			continue;
		QFile file(existing.absoluteFilePath());
		if(file.open(QIODevice::ReadOnly) && file.readAll() == content)
			return;
	}

	QDir().mkpath(folder);
	QString filename = folder + "/" + name;
	if(QFileInfo::exists(filename))
		filename = folder + "/" + stem + " (" + Timestamp() + ")" + suffix;
	WriteFile(filename, data);
}
