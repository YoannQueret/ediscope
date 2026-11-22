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

#include "spi_decoder.h"

#include <cmath>
#include <cstdio>
#include <ctime>

#include "tools.h"


namespace {

// ETSI TS 102 371 clause 5.2: elements (tag < 0x80, Annex D) and attributes (tag 0x01, 0x04 or
// >= 0x80, Annex E) share the exact same tag/length(+extended)/data binary structure (clauses
// 5.2.3/5.2.4), so a single generic walker serves for both - which one a given tag is just
// depends on which element it was found inside (there's no structural marker, only convention)
struct TLVNode {
	int tag;
	const uint8_t *data;
	size_t len;
};

std::vector<TLVNode> WalkTLV(const uint8_t *data, size_t len) {
	std::vector<TLVNode> nodes;
	size_t offset = 0;

	while(offset + 2 <= len) {
		int tag = data[offset];
		size_t node_len = data[offset + 1];
		offset += 2;

		if(node_len == 0xFE) {
			if(offset + 2 > len)
				break;
			node_len = (size_t) data[offset] << 8 | data[offset + 1];
			offset += 2;
		} else if(node_len == 0xFF) {
			if(offset + 3 > len)
				break;
			node_len = (size_t) data[offset] << 16 | data[offset + 1] << 8 | data[offset + 2];
			offset += 3;
		}

		if(offset + node_len > len)
			break;

		TLVNode node;
		node.tag = tag;
		node.data = data + offset;
		node.len = node_len;
		nodes.push_back(node);

		offset += node_len;
	}

	return nodes;
}

typedef std::map<uint8_t,std::string> token_table_t;

// ETSI TS 102 371 clause 5.5.2 - a sequence of (token_tag, token_length, token_data) triples,
// using a plain 1-byte length (no 0xFE/0xFF extension, unlike the generic TLV above)
token_table_t ParseTokenTable(const uint8_t *data, size_t len) {
	token_table_t tokens;
	size_t offset = 0;

	while(offset + 2 <= len) {
		uint8_t tag = data[offset];
		size_t token_len = data[offset + 1];
		offset += 2;

		if(offset + token_len > len)
			break;

		tokens[tag] = std::string((const char *) data + offset, token_len);
		offset += token_len;
	}

	return tokens;
}

// substitutes any token tag byte found within a string's raw bytes with its token table string
// (clause 5.5.1); the data is already UTF-8 (clause 5.4.2) otherwise
std::string DecodeString(const uint8_t *data, size_t len, const token_table_t& tokens) {
	std::string result;
	result.reserve(len);

	for(size_t i = 0; i < len; i++) {
		token_table_t::const_iterator it = tokens.find(data[i]);
		if(it != tokens.end())
			result += it->second;
		else
			result += (char) data[i];
	}

	return result;
}

// shortName/mediumName/longName (clause 5.3.1): an optional xml:lang string attribute (0x80),
// followed by the name itself as CDATA (attribute tag 0x01)
void ParseNameElement(const TLVNode& node, const token_table_t& tokens, std::map<std::string,std::string>& out) {
	std::string lang;
	std::string text;

	for(const TLVNode& child : WalkTLV(node.data, node.len)) {
		if(child.tag == 0x80)
			lang = DecodeString(child.data, child.len, tokens);
		else if(child.tag == 0x01)
			text = DecodeString(child.data, child.len, tokens);
	}

	out[lang] = text;
}

// multimedia (Annex E table E.1) - a logo (or other media) reference
void ParseMultimedia(const TLVNode& node, const token_table_t& tokens, std::vector<SPI_LOGO>& out) {
	SPI_LOGO logo;

	for(const TLVNode& attr : WalkTLV(node.data, node.len)) {
		switch(attr.tag) {
		case 0x80:	// mimeValue
			logo.mime = DecodeString(attr.data, attr.len, tokens);
			break;
		case 0x82:	// url - for broadcast delivery, actually the referenced MOT object's
					// ContentName (ETSI TS 102 371 clause 6.5), never a real URL
			logo.content_name = DecodeString(attr.data, attr.len, tokens);
			break;
		case 0x83:	// type (Annex F enumerated)
			if(attr.len >= 1)
				logo.type = attr.data[0];
			break;
		case 0x84:	// width
			if(attr.len >= 2)
				logo.width = attr.data[0] << 8 | attr.data[1];
			break;
		case 0x85:	// height
			if(attr.len >= 2)
				logo.height = attr.data[0] << 8 | attr.data[1];
			break;
		}
	}

	out.push_back(logo);
}

void ParseMediaDescription(const TLVNode& node, const token_table_t& tokens, std::vector<SPI_LOGO>& out) {
	for(const TLVNode& child : WalkTLV(node.data, node.len))
		if(child.tag == 0x2B)	// multimedia
			ParseMultimedia(child, tokens, out);
}

// the dab: domain bearerURI "id" encoding (clause 5.4.5.1.2), shared by the bearer element (SI,
// clause 5.3.4) and the serviceScope element (PI, clause 5.3.3) - both use attribute tag 0x80 for it
bool DecodeDABBearerId(const uint8_t *data, size_t len, int& ecc, int& eid, uint32_t& sid, bool& sid_32bit, int& scids) {
	if(len < 4)
		return false;

	sid_32bit = data[0] & 0x10;
	scids = data[0] & 0x0F;
	ecc = data[1];
	eid = data[2] << 8 | data[3];

	if(sid_32bit) {
		if(len < 8)
			return false;
		sid = (uint32_t) data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
	} else {
		if(len < 6)
			return false;
		sid = data[4] << 8 | data[5];
	}

	return true;
}

// bearer (Annex E table E.1) - only the dab: domain id encoding is handled; an http: domain
// bearer (url attribute, tag 0x82) is out of scope for v1
void ParseBearer(const TLVNode& node, SPI_SERVICE& service) {
	for(const TLVNode& attr : WalkTLV(node.data, node.len)) {
		if(attr.tag != 0x80)
			continue;
		service.has_dab_bearer = DecodeDABBearerId(attr.data, attr.len, service.ecc, service.eid, service.sid, service.sid_32bit, service.scids);
	}
}

// serviceScope (clause 5.3.3) - identifies which service a <schedule> (PI) object is for
void ParseServiceScope(const TLVNode& node, SPI_SCHEDULE& schedule) {
	for(const TLVNode& attr : WalkTLV(node.data, node.len)) {
		if(attr.tag != 0x80)
			continue;
		schedule.has_scope_service = DecodeDABBearerId(attr.data, attr.len, schedule.ecc, schedule.eid, schedule.sid, schedule.sid_32bit, schedule.scids);
	}
}

// timepoint type (clause 5.4.5.2) - MJD date (identical bit layout to the FIC's own FIG 0/10) plus
// UTC time (short or long form) and an optional LTO, encoded as follows:
//   byte0: 1 bit Rfa, 17-bit Date (bits 16-10)
//   byte1: Date (bits 9-2)
//   byte2: Date (bits 1-0), 1 bit Rfa, LTOflag, UTCflag, UTC (top 3 bits)
//   byte3[-5]: rest of UTC (8, or 8+8+8 for the long form: hours/minutes[/seconds+10 bits Rfa])
//   [+1 byte LTO, if LTOflag]: 2 bits Rfa, sign, 5-bit half-hour count
bool DecodeTimepoint(const uint8_t *data, size_t len, FIC_DAB_DT& out_utc, int& out_lto) {
	if(len < 4)
		return false;

	int mjd = (data[0] & 0x7F) << 10 | data[1] << 2 | data[2] >> 6;

	// same Julian-to-calendar conversion as FICDecoder::ProcessFIG0_10
	int y0 = floor((mjd - 15078.2) / 365.25);
	int m0 = floor((mjd - 14956.1 - floor(y0 * 365.25)) / 30.6001);
	int d = mjd - 14956 - floor(y0 * 365.25) - floor(m0 * 30.6001);
	int k = (m0 == 14 || m0 == 15) ? 1 : 0;
	int y = y0 + k;
	int m = m0 - 1 - k * 12;

	out_utc = FIC_DAB_DT();
	out_utc.dt.tm_year = y;
	out_utc.dt.tm_mon = m - 1;
	out_utc.dt.tm_mday = d;
	out_utc.dt.tm_isdst = -1;

	bool lto_flag = data[2] & 0x10;
	bool utc_flag = data[2] & 0x08;

	size_t offset = 3;
	if(utc_flag) {
		// long form: 27-bit UTC (hours/minutes/seconds/10-bit Rfa)
		if(len < offset + 3)
			return false;
		uint32_t utc27 = (uint32_t) (data[2] & 0x07) << 24 | data[offset] << 16 | data[offset + 1] << 8 | data[offset + 2];
		out_utc.dt.tm_hour = (utc27 >> 22) & 0x1F;
		out_utc.dt.tm_min = (utc27 >> 16) & 0x3F;
		out_utc.dt.tm_sec = (utc27 >> 10) & 0x3F;
		out_utc.ms = FIC_DAB_DT::ms_none;
		offset += 3;
	} else {
		// short form: 11-bit UTC (hours/minutes)
		if(len < offset + 1)
			return false;
		int utc11 = (data[2] & 0x07) << 8 | data[offset];
		out_utc.dt.tm_hour = (utc11 >> 6) & 0x1F;
		out_utc.dt.tm_min = utc11 & 0x3F;
		out_utc.dt.tm_sec = 0;
		out_utc.ms = FIC_DAB_DT::ms_none;
		offset += 1;
	}

	if(lto_flag) {
		if(len < offset + 1)
			return false;
		out_lto = (data[offset] & 0x20 ? -1 : 1) * (data[offset] & 0x1F);
	} else {
		out_lto = FIC_ENSEMBLE::lto_none;
	}

	return true;
}

// location (clause 5.3.5) - only the first <time> child's planned time/duration is kept (clause
// 5.4.5.2/5.4.5.3); actualTime/actualDuration (real-time deviation tracking) and bearer/relativeTime
// children are out of scope for v1
void ParseLocation(const TLVNode& node, SPI_TIME& out) {
	for(const TLVNode& child : WalkTLV(node.data, node.len)) {
		if(child.tag != 0x2C)	// time
			continue;

		for(const TLVNode& attr : WalkTLV(child.data, child.len)) {
			if(attr.tag == 0x80)	// time
				out.valid = DecodeTimepoint(attr.data, attr.len, out.utc, out.lto);
			else if(attr.tag == 0x81 && attr.len >= 2)	// duration
				out.duration_seconds = attr.data[0] << 8 | attr.data[1];
		}
		return;
	}
}

void ParseProgramme(const TLVNode& node, const token_table_t& tokens, std::vector<SPI_PROGRAMME>& out) {
	SPI_PROGRAMME prog;

	for(const TLVNode& child : WalkTLV(node.data, node.len)) {
		switch(child.tag) {
		case 0x81:	// shortId (24-bit)
			if(child.len >= 3)
				prog.short_id = child.data[0] << 16 | child.data[1] << 8 | child.data[2];
			break;
		case 0x84:	// broadcast (enumerated)
			if(child.len >= 1)
				prog.broadcast = child.data[0];
			break;
		case 0x10:
			ParseNameElement(child, tokens, prog.short_names);
			break;
		case 0x11:
			ParseNameElement(child, tokens, prog.medium_names);
			break;
		case 0x12:
			ParseNameElement(child, tokens, prog.long_names);
			break;
		case 0x19:	// location
			ParseLocation(child, prog.time);
			break;
		}
	}

	out.push_back(prog);
}

void ParseProgrammeGroup(const TLVNode& node, const token_table_t& tokens, std::vector<SPI_PROGRAMME_GROUP>& out) {
	SPI_PROGRAMME_GROUP group;

	for(const TLVNode& child : WalkTLV(node.data, node.len)) {
		switch(child.tag) {
		case 0x80:	// id
			group.id = DecodeString(child.data, child.len, tokens);
			break;
		case 0x81:	// shortId
			if(child.len >= 3)
				group.short_id = child.data[0] << 16 | child.data[1] << 8 | child.data[2];
			break;
		case 0x83:	// type (enumerated)
			if(child.len >= 1)
				group.type = child.data[0];
			break;
		case 0x84:	// numOfItems
			if(child.len >= 2)
				group.num_of_items = child.data[0] << 8 | child.data[1];
			break;
		case 0x10:
			ParseNameElement(child, tokens, group.short_names);
			break;
		case 0x11:
			ParseNameElement(child, tokens, group.medium_names);
			break;
		case 0x12:
			ParseNameElement(child, tokens, group.long_names);
			break;
		case 0x13:
			ParseMediaDescription(child, tokens, group.logos);
			break;
		}
	}

	out.push_back(group);
}

void ParseService(const TLVNode& node, const token_table_t& tokens, SPI_SERVICE_INFORMATION& out) {
	SPI_SERVICE service;

	for(const TLVNode& child : WalkTLV(node.data, node.len)) {
		switch(child.tag) {
		case 0x10:
			ParseNameElement(child, tokens, service.short_names);
			break;
		case 0x11:
			ParseNameElement(child, tokens, service.medium_names);
			break;
		case 0x12:
			ParseNameElement(child, tokens, service.long_names);
			break;
		case 0x13:
			ParseMediaDescription(child, tokens, service.logos);
			break;
		case 0x29:
			ParseBearer(child, service);
			break;
		}
	}

	out.services.push_back(service);
}

// ensemble (clause 5.3.2.3) - only present for a DAB delivery system; its id attribute carries
// the ECC/EId, and it's the actual parent of the service elements (NOTE 1 of clause 5.3.2.2)
void ParseEnsemble(const TLVNode& node, const token_table_t& tokens, SPI_SERVICE_INFORMATION& out) {
	for(const TLVNode& child : WalkTLV(node.data, node.len)) {
		switch(child.tag) {
		case 0x80:	// id - ECC(8) + EId(16)
			if(child.len >= 3) {
				out.ensemble_ecc = child.data[0];
				out.ensemble_eid = child.data[1] << 8 | child.data[2];
			}
			break;
		case 0x10:
			ParseNameElement(child, tokens, out.ensemble_short_names);
			break;
		case 0x11:
			ParseNameElement(child, tokens, out.ensemble_medium_names);
			break;
		case 0x12:
			ParseNameElement(child, tokens, out.ensemble_long_names);
			break;
		case 0x13:
			ParseMediaDescription(child, tokens, out.ensemble_logos);
			break;
		case 0x28:
			ParseService(child, tokens, out);
			break;
		}
	}
}

// scope (clause 5.3: schedule -> scope -> serviceScope) - identifies the service a schedule
// describes; startTime/stopTime (the schedule's overall validity window) aren't kept for v1
void ParseScope(const TLVNode& node, SPI_SCHEDULE& schedule) {
	for(const TLVNode& child : WalkTLV(node.data, node.len))
		if(child.tag == 0x25)	// serviceScope
			ParseServiceScope(child, schedule);
}

void ParseSchedule(const TLVNode& node, const token_table_t& tokens, SPI_SCHEDULE& out) {
	for(const TLVNode& child : WalkTLV(node.data, node.len)) {
		switch(child.tag) {
		case 0x24:	// scope
			ParseScope(child, out);
			break;
		case 0x1C:	// programme
			ParseProgramme(child, tokens, out.programmes);
			break;
		}
	}
}

void ParseProgrammeGroups(const TLVNode& node, const token_table_t& tokens, SPI_GROUP_INFORMATION& out) {
	for(const TLVNode& child : WalkTLV(node.data, node.len))
		if(child.tag == 0x23)	// programmeGroup
			ParseProgrammeGroup(child, tokens, out.groups);
}

std::string EscapeXML(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for(char c : s) {
		switch(c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		case '"':
			out += "&quot;";
			break;
		default:
			out += c;
		}
	}
	return out;
}

void AppendNames(std::string& xml, const std::string& indent, const char *tag, const std::map<std::string,std::string>& names) {
	for(const std::pair<const std::string,std::string>& kv : names) {
		xml += indent + "<" + tag;
		if(!kv.first.empty())
			xml += " xml:lang=\"" + EscapeXML(kv.first) + "\"";
		xml += ">" + EscapeXML(kv.second) + "</" + tag + ">\n";
	}
}

std::string FormatBearerId(int ecc, int eid, uint32_t sid, bool sid_32bit, int scids) {
	return "dab:" +
			StringTools::IntToHex(ecc, 2) + "." +
			StringTools::IntToHex(eid, 4) + "." +
			StringTools::IntToHex((int) sid, sid_32bit ? 8 : 4) + "." +
			std::to_string(scids);
}

std::string FormatSPITime(const SPI_TIME& time) {
	if(!time.valid)
		return "";

	std::string s = FICDecoder::ConvertDateTimeToString(time.utc, time.lto == FIC_ENSEMBLE::lto_none ? 0 : time.lto, false);
	if(time.duration_seconds >= 0)
		s += " (+" + std::to_string(time.duration_seconds / 60) + " min)";
	return s;
}

void AppendLogos(std::string& xml, const std::string& indent, const std::vector<SPI_LOGO>& logos) {
	if(logos.empty())
		return;

	xml += indent + "<mediaDescription>\n";
	for(const SPI_LOGO& logo : logos) {
		xml += indent + "  <multimedia url=\"" + EscapeXML(logo.content_name) + "\"";
		switch(logo.type) {
		case SPI_LOGO::LOGO_TYPE_COLOUR_SQUARE:
			xml += " type=\"logo_colour_square\"";
			break;
		case SPI_LOGO::LOGO_TYPE_COLOUR_RECTANGLE:
			xml += " type=\"logo_colour_rectangle\"";
			break;
		case SPI_LOGO::LOGO_TYPE_UNRESTRICTED:
			xml += " type=\"logo_unrestricted\"";
			if(!logo.mime.empty())
				xml += " mimeValue=\"" + EscapeXML(logo.mime) + "\"";
			if(logo.width > 0)
				xml += " width=\"" + std::to_string(logo.width) + "\"";
			if(logo.height > 0)
				xml += " height=\"" + std::to_string(logo.height) + "\"";
			break;
		default:
			break;
		}
		xml += "/>\n";
	}
	xml += indent + "</mediaDescription>\n";
}

} // namespace


std::string SPIDecoder::ToXML(const SPI_SERVICE_INFORMATION& info) {
	std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<serviceInformation>\n  <ensemble";

	if(info.ensemble_ecc >= 0 && info.ensemble_eid >= 0)
		xml += " id=\"dab:" + StringTools::IntToHex(info.ensemble_ecc, 2) + "." + StringTools::IntToHex(info.ensemble_eid, 4) + "\"";
	xml += ">\n";

	AppendNames(xml, "    ", "shortName", info.ensemble_short_names);
	AppendNames(xml, "    ", "mediumName", info.ensemble_medium_names);
	AppendNames(xml, "    ", "longName", info.ensemble_long_names);
	AppendLogos(xml, "    ", info.ensemble_logos);

	for(const SPI_SERVICE& service : info.services) {
		xml += "    <service>\n";
		AppendNames(xml, "      ", "shortName", service.short_names);
		AppendNames(xml, "      ", "mediumName", service.medium_names);
		AppendNames(xml, "      ", "longName", service.long_names);
		AppendLogos(xml, "      ", service.logos);
		if(service.has_dab_bearer)
			xml += "      <bearer id=\"" + FormatBearerId(service.ecc, service.eid, service.sid, service.sid_32bit, service.scids) + "\"/>\n";
		xml += "    </service>\n";
	}

	xml += "  </ensemble>\n</serviceInformation>\n";
	return xml;
}

bool SPIDecoder::DecodeServiceInformation(const std::vector<uint8_t>& data, SPI_SERVICE_INFORMATION& out) {
	std::vector<TLVNode> top = WalkTLV(data.data(), data.size());
	if(top.size() != 1 || top[0].tag != 0x03) {
		fprintf(stderr, "SPIDecoder: not a single top-level <serviceInformation> (0x03) binary object - ignored\n");
		return false;
	}

	const TLVNode& si = top[0];
	std::vector<TLVNode> children = WalkTLV(si.data, si.len);

	// the token table (if any) applies to the whole binary object (clause 5.5.1), so it has to
	// be known before any string below it is decoded, regardless of where exactly it appears
	token_table_t tokens;
	for(const TLVNode& child : children)
		if(child.tag == 0x04)
			tokens = ParseTokenTable(child.data, child.len);

	for(const TLVNode& child : children) {
		if(child.tag == 0x26)	// ensemble (DAB)
			ParseEnsemble(child, tokens, out);
		else if(child.tag == 0x28)	// service, directly under serviceInformation (DRM only)
			ParseService(child, tokens, out);
	}

	return true;
}

bool SPIDecoder::DecodeScopeId(const std::vector<uint8_t>& scope_id, int& ecc, int& eid, uint32_t& sid, bool& sid_32bit, int& scids) {
	if(scope_id.empty())
		return false;
	return DecodeDABBearerId(scope_id.data(), scope_id.size(), ecc, eid, sid, sid_32bit, scids);
}

bool SPIDecoder::DecodeScopeTime(const std::vector<uint8_t>& raw, SPI_TIME& out) {
	if(raw.empty())
		return false;
	out.valid = DecodeTimepoint(raw.data(), raw.size(), out.utc, out.lto);
	return out.valid;
}

std::string SPIDecoder::FormatDate(const SPI_TIME& time) {
	if(!time.valid)
		return "";

	struct tm local_tm = time.utc.dt;
	if(time.lto != FIC_ENSEMBLE::lto_none)
		local_tm.tm_min += time.lto * 30;
	local_tm.tm_isdst = -1;
	if(mktime(&local_tm) == (time_t) -1)
		return "";

	char buf[11];
	strftime(buf, sizeof(buf), "%Y-%m-%d", &local_tm);
	return buf;
}

bool SPIDecoder::DecodeSchedule(const std::vector<uint8_t>& data, SPI_SCHEDULE& out) {
	// PI objects are encoded as <epg> (0x02, clause 5.2.2.1's other top-level element, alongside
	// serviceInformation) with a single <schedule> (0x21) child - not <schedule> directly, despite
	// clause 6.3.1 describing a PI "document" as containing one, which only describes its XML/
	// master-document shape, not this wire-level wrapping
	std::vector<TLVNode> top = WalkTLV(data.data(), data.size());
	if(top.size() != 1 || top[0].tag != 0x02) {
		fprintf(stderr, "SPIDecoder: not a single top-level <epg> (0x02) binary object - ignored\n");
		return false;
	}

	std::vector<TLVNode> epg_children = WalkTLV(top[0].data, top[0].len);

	token_table_t tokens;
	for(const TLVNode& child : epg_children)
		if(child.tag == 0x04)
			tokens = ParseTokenTable(child.data, child.len);

	for(const TLVNode& child : epg_children)
		if(child.tag == 0x21)
			ParseSchedule(child, tokens, out);

	return true;
}

bool SPIDecoder::DecodeGroupInformation(const std::vector<uint8_t>& data, SPI_GROUP_INFORMATION& out) {
	// GI objects are likewise <epg> (0x02) with a single <programmeGroups> (0x20) child
	std::vector<TLVNode> top = WalkTLV(data.data(), data.size());
	if(top.size() != 1 || top[0].tag != 0x02) {
		fprintf(stderr, "SPIDecoder: not a single top-level <epg> (0x02) binary object - ignored\n");
		return false;
	}

	std::vector<TLVNode> epg_children = WalkTLV(top[0].data, top[0].len);

	token_table_t tokens;
	for(const TLVNode& child : epg_children)
		if(child.tag == 0x04)
			tokens = ParseTokenTable(child.data, child.len);

	for(const TLVNode& child : epg_children)
		if(child.tag == 0x20)
			ParseProgrammeGroups(child, tokens, out);

	return true;
}

std::string SPIDecoder::ToXML(const SPI_SCHEDULE& schedule) {
	std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<epg>\n  <schedule>\n";

	if(schedule.has_scope_service) {
		xml += "    <scope>\n      <serviceScope id=\"" +
				FormatBearerId(schedule.ecc, schedule.eid, schedule.sid, schedule.sid_32bit, schedule.scids) +
				"\"/>\n    </scope>\n";
	}

	for(const SPI_PROGRAMME& prog : schedule.programmes) {
		xml += "    <programme";
		if(prog.short_id >= 0)
			xml += " shortId=\"" + std::to_string(prog.short_id) + "\"";
		if(prog.broadcast == SPI_PROGRAMME::BROADCAST_ON_AIR)
			xml += " broadcast=\"on-air\"";
		else if(prog.broadcast == SPI_PROGRAMME::BROADCAST_OFF_AIR)
			xml += " broadcast=\"off-air\"";
		xml += ">\n";

		AppendNames(xml, "      ", "shortName", prog.short_names);
		AppendNames(xml, "      ", "mediumName", prog.medium_names);
		AppendNames(xml, "      ", "longName", prog.long_names);

		if(prog.time.valid) {
			xml += "      <location>\n        <time time=\"" + FormatSPITime(prog.time) + "\"";
			if(prog.time.duration_seconds >= 0)
				xml += " duration=\"" + std::to_string(prog.time.duration_seconds) + "\"";
			xml += "/>\n      </location>\n";
		}

		xml += "    </programme>\n";
	}

	xml += "  </schedule>\n</epg>\n";
	return xml;
}

std::string SPIDecoder::ToXML(const SPI_GROUP_INFORMATION& info) {
	std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<epg>\n  <programmeGroups>\n";

	for(const SPI_PROGRAMME_GROUP& group : info.groups) {
		xml += "    <programmeGroup";
		if(!group.id.empty())
			xml += " id=\"" + EscapeXML(group.id) + "\"";
		if(group.short_id >= 0)
			xml += " shortId=\"" + std::to_string(group.short_id) + "\"";
		if(group.num_of_items >= 0)
			xml += " numOfItems=\"" + std::to_string(group.num_of_items) + "\"";
		xml += ">\n";

		AppendNames(xml, "      ", "shortName", group.short_names);
		AppendNames(xml, "      ", "mediumName", group.medium_names);
		AppendNames(xml, "      ", "longName", group.long_names);
		AppendLogos(xml, "      ", group.logos);

		xml += "    </programmeGroup>\n";
	}

	xml += "  </programmeGroups>\n</epg>\n";
	return xml;
}
