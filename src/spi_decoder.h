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

#ifndef SPI_DECODER_H_
#define SPI_DECODER_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "fic_decoder.h"	// reuses FIC_DAB_DT + FICDecoder::ConvertDateTimeToString/ConvertLTOToString - the
							// SPI timepoint type (clause 5.4.5.2) is explicitly the same UTC+LTO scheme as the FIC's own


// --- SPI_LOGO -----------------------------------------------------------------
// one <multimedia> (logo) reference (ETSI TS 102 371 clause 5.3.4/Annex E, ETSI TS 102 818
// clause 5.8/Annex D). For a broadcast-delivered logo, content_name is the MOT ContentName of
// the image object carrying its actual pixel data (carried in the same MOT carousel), NOT a
// real URL - see ETSI TS 102 371 clause 6.5's own note on this.
struct SPI_LOGO {
	int type;	// LOGO_TYPE_* below, or -1 if absent/unrecognised
	std::string content_name;
	int width;
	int height;
	std::string mime;

	static const int LOGO_TYPE_UNRESTRICTED = 0x02;
	static const int LOGO_TYPE_COLOUR_SQUARE = 0x04;
	static const int LOGO_TYPE_COLOUR_RECTANGLE = 0x06;

	SPI_LOGO() : type(-1), width(0), height(0) {}
};

// --- SPI_SERVICE -----------------------------------------------------------------
struct SPI_SERVICE {
	// the DAB bearer this entry refers to (ETSI TS 102 371 clause 5.4.5.1.2) - matched against
	// the ensemble's own ECC/EId and a service's SId/SCIdS to know which row it describes
	bool has_dab_bearer;
	int ecc;
	int eid;
	uint32_t sid;
	bool sid_32bit;
	int scids;

	// name, per xml:lang ("" = no language given/default)
	std::map<std::string,std::string> short_names;
	std::map<std::string,std::string> medium_names;
	std::map<std::string,std::string> long_names;

	std::vector<SPI_LOGO> logos;

	SPI_SERVICE() : has_dab_bearer(false), ecc(-1), eid(-1), sid(0), sid_32bit(false), scids(-1) {}
};

// --- SPI_SERVICE_INFORMATION -----------------------------------------------------------------
struct SPI_SERVICE_INFORMATION {
	int ensemble_ecc;
	int ensemble_eid;
	std::map<std::string,std::string> ensemble_short_names;
	std::map<std::string,std::string> ensemble_medium_names;
	std::map<std::string,std::string> ensemble_long_names;
	std::vector<SPI_LOGO> ensemble_logos;

	std::vector<SPI_SERVICE> services;

	SPI_SERVICE_INFORMATION() : ensemble_ecc(-1), ensemble_eid(-1) {}
};


// --- SPI_TIME -----------------------------------------------------------------
// a decoded timepoint (ETSI TS 102 371 clause 5.4.5.2) + optional duration (clause 5.4.5.3)
struct SPI_TIME {
	bool valid;
	FIC_DAB_DT utc;
	int lto;				// half-hour units, signed (same convention as FIC_ENSEMBLE::lto) - FIC_ENSEMBLE::lto_none if absent
	int duration_seconds;	// -1 if absent

	SPI_TIME() : valid(false), lto(FIC_ENSEMBLE::lto_none), duration_seconds(-1) {}
};

// --- SPI_PROGRAMME -----------------------------------------------------------------
struct SPI_PROGRAMME {
	int short_id;	// 24-bit, -1 if absent
	std::map<std::string,std::string> short_names;
	std::map<std::string,std::string> medium_names;
	std::map<std::string,std::string> long_names;
	SPI_TIME time;	// from the first <location><time> found

	static const int BROADCAST_ON_AIR = 0x01;
	static const int BROADCAST_OFF_AIR = 0x02;
	int broadcast;	// BROADCAST_* above, -1 if absent

	SPI_PROGRAMME() : short_id(-1), broadcast(-1) {}
};

// --- SPI_SCHEDULE -----------------------------------------------------------------
// one <schedule> binary object (ContentSubType 1, Programme Information) - per ETSI TS 102 371
// clause 6.3.1, one PI object exists per service per day
struct SPI_SCHEDULE {
	// the service this schedule is for (<scope><serviceScope id="dab:...">), same DAB bearer
	// encoding as SPI_SERVICE's own bearer
	bool has_scope_service;
	int ecc;
	int eid;
	uint32_t sid;
	bool sid_32bit;
	int scids;

	std::vector<SPI_PROGRAMME> programmes;

	// the day this schedule covers (ETSI TS 102 371 clauses 6.4.7/6.4.8, MOT parameters
	// ScopeStart/ScopeEnd - a PI object is "per service per day", clause 6.3.1) - scope_start is
	// the authoritative source for which day this is; falls back to the first programme's own
	// time if an encoder omits it (not seen yet, but not guaranteed either)
	SPI_TIME scope_start;
	SPI_TIME scope_end;

	SPI_SCHEDULE() : has_scope_service(false), ecc(-1), eid(-1), sid(0), sid_32bit(false), scids(-1) {}
};

// --- SPI_PROGRAMME_GROUP -----------------------------------------------------------------
struct SPI_PROGRAMME_GROUP {
	std::string id;
	int short_id;	// -1 if absent
	int num_of_items;	// -1 if absent

	static const int TYPE_SERIES = 0x02;
	static const int TYPE_SHOW = 0x03;
	static const int TYPE_PROGRAM_CONCEPT = 0x04;
	static const int TYPE_MAGAZINE = 0x05;
	static const int TYPE_PROGRAM_COMPILATION = 0x06;
	static const int TYPE_OTHER_COLLECTION = 0x07;
	static const int TYPE_OTHER_CHOICE = 0x08;
	static const int TYPE_TOPIC = 0x09;
	int type;	// TYPE_* above, -1 if absent

	std::map<std::string,std::string> short_names;
	std::map<std::string,std::string> medium_names;
	std::map<std::string,std::string> long_names;
	std::vector<SPI_LOGO> logos;

	SPI_PROGRAMME_GROUP() : short_id(-1), num_of_items(-1), type(-1) {}
};

// --- SPI_GROUP_INFORMATION -----------------------------------------------------------------
// one <programmeGroups> binary object (ContentSubType 2, Group Information) - per ETSI TS 102 371
// clause 6.3.1, one GI object exists per ensemble
struct SPI_GROUP_INFORMATION {
	std::vector<SPI_PROGRAMME_GROUP> groups;
};


// --- SPIDecoder -----------------------------------------------------------------
// Decodes a single ETSI TS 102 371 clause 5 binary object carrying a top-level
// <serviceInformation> element (ContentType=application/0x07, ContentSubType=0, per clause
// 6.4.2 table 11) into the structures above. Decodes whatever Basic/Advanced profile elements
// and attributes it finds generically - it doesn't enforce or care which profile produced them.
class SPIDecoder {
public:
	// returns false if the object doesn't even parse as a well-formed <serviceInformation>
	// binary object (e.g. wrong top-level tag, truncated data) - a best-effort partial result
	// is still filled in whenever an individual element/attribute is malformed, matching this
	// format's own "ignore anything you don't understand" philosophy
	static bool DecodeServiceInformation(const std::vector<uint8_t>& data, SPI_SERVICE_INFORMATION& out);

	// ContentSubType 1 (Programme Information) - a <schedule> binary object
	static bool DecodeSchedule(const std::vector<uint8_t>& data, SPI_SCHEDULE& out);

	// ContentSubType 2 (Group Information) - a <programmeGroups> binary object
	static bool DecodeGroupInformation(const std::vector<uint8_t>& data, SPI_GROUP_INFORMATION& out);

	// decodes a MOT ScopeID parameter (ETSI TS 102 371 clause 6.4.9) for a PI object - the same
	// bearerURI "id" encoding as clause 5.4.5.1.2, just without a domain prefix (always dab:,
	// implicitly, since there is no other delivery system here) - some encoders identify a
	// schedule's service this way instead of (or as well as) a <scope><serviceScope> in its content
	static bool DecodeScopeId(const std::vector<uint8_t>& scope_id, int& ecc, int& eid, uint32_t& sid, bool& sid_32bit, int& scids);

	// decodes a MOT ScopeStart/ScopeEnd parameter (timepoint type, short form + optional LTO only)
	static bool DecodeScopeTime(const std::vector<uint8_t>& raw, SPI_TIME& out);

	// "YYYY-MM-DD", in the same local time as displayed elsewhere (applies time.lto) - "" if invalid
	static std::string FormatDate(const SPI_TIME& time);

	// reconstructs readable XML text from the decoded structures above, for inspection - this is
	// NOT the original master document byte-for-byte (the wire format is a proprietary binary
	// tree, not XML - see clause 5), just a faithful rendering of everything this decoder itself
	// extracted from it
	static std::string ToXML(const SPI_SERVICE_INFORMATION& info);
	static std::string ToXML(const SPI_SCHEDULE& schedule);
	static std::string ToXML(const SPI_GROUP_INFORMATION& info);
};

#endif /* SPI_DECODER_H_ */
