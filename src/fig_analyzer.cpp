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

#include "fig_analyzer.h"

#include <algorithm>
#include <cstdio>

#include "fic_decoder.h"
#include "tools.h"


static const double FRAME_MS = 24.0;
// the FIB time quantum (3 FIBs per 24 ms frame in transmission mode I): a FIG's position within
// the 96 ms cycle may move by up to this much without its repetition really being any slower
static const double TOLERANCE_MS = 8.0;
static const size_t MAX_EVENTS = 500;

// the database keys used below pack their fields into 64 bits - the FIG/check it belongs to is
// known from the map it is stored in
static uint64_t Key(uint64_t a, uint64_t b = 0, uint64_t c = 0) {return a << 40 | b << 20 | c;}

static std::string Hex(uint32_t value, int nibbles) {
	char buf[16];
	snprintf(buf, sizeof(buf), "%0*X", nibbles, value);
	return buf;
}


// --- FIGGapStats -----------------------------------------------------------------
void FIGGapStats::Add(double t_ms) {
	if(count == 0) {
		first_ms = t_ms;
	} else {
		double gap = t_ms - last_ms;
		if(count == 1 || gap < min_gap_ms)
			min_gap_ms = gap;
		if(gap > max_gap_ms)
			max_gap_ms = gap;
		sum_gap_ms += gap;
	}
	last_ms = t_ms;
	count++;
}


// --- FIGHistogram -----------------------------------------------------------------
const double FIGHistogram::limits[FIGHistogram::classes - 1] = {1, 1.5, 2, 3, 5, 10};

void FIGHistogram::Add(double gap_ms, double nominal_ms) {
	// the same FIB time quantum tolerance as the verdicts, so "<= 1x" and "<= 3x" match them
	int c = 0;
	while(c < classes - 1 && gap_ms > limits[c] * nominal_ms + TOLERANCE_MS)
		c++;
	counts[c]++;
}

void FIGHistogram::Merge(const FIGHistogram& other) {
	for(int c = 0; c < classes; c++)
		counts[c] += other.counts[c];
}

uint64_t FIGHistogram::Total() const {
	uint64_t total = 0;
	for(int c = 0; c < classes; c++)
		total += counts[c];
	return total;
}


// --- FIGAnalyzer -----------------------------------------------------------------
FIGAnalyzer::FIGAnalyzer() {
	Reset();
}

void FIGAnalyzer::AddCheck(const std::string& id, const std::string& fig, const std::string& usage, const std::string& requirement, CheckKind kind, double nominal_ms, double burst_window_ms) {
	Check& check = checks[id];
	check.fig = fig;
	check.usage = usage;
	check.requirement = requirement;
	check.kind = kind;
	check.nominal_ms = nominal_ms;
	check.burst_window_ms = burst_window_ms;
	check.events = 0;
	check.order = (int) checks.size();
}

void FIGAnalyzer::InitChecks() {
	// ETSI TS 103 176 V2.6.1 clause 4: MCI every 96 ms (FIG 0/8 for data components and FIG 0/13:
	// at least every 1 s), SI once per second; table 1 for the complex SI FIGs
	const CheckKind P = CheckKind::PERIODIC;
	const CheckKind B = CheckKind::BURST;
	AddCheck("0/0",    "FIG 0/0",  "MCI, ensemble information",                   "every 96 ms", P, 96);
	AddCheck("0/1",    "FIG 0/1",  "MCI, per sub-channel",                        "every 96 ms", P, 96);
	AddCheck("0/2",    "FIG 0/2",  "MCI, per service",                            "every 96 ms", P, 96);
	AddCheck("0/3",    "FIG 0/3",  "MCI, per packet mode component (SCId)",       "every 96 ms", P, 96);
	AddCheck("0/5",    "FIG 0/5",  "SI, language per component",                  "once per second", P, 1000);
	AddCheck("0/6db",  "FIG 0/6",  "Long form, database definition",              "all information at least every 2 minutes", P, 120000);
	AddCheck("0/6act", "FIG 0/6",  "Activation state of each linkage set",        "at least every 10 seconds", P, 10000);
	AddCheck("0/6cei", "FIG 0/6",  "Short form, CEI",                             "once per second for 5 seconds", B, 1000, 5000);
	AddCheck("0/7",    "FIG 0/7",  "MCI, configuration information",              "every 96 ms", P, 96);
	// clause 4 asks for 96 ms here (only data components at 1 s), but the compliant example of
	// annex F signals FIG 0/8 for every component at 1 s - the latter is followed
	AddCheck("0/8p",   "FIG 0/8",  "MCI, per programme service component",        "at least every 1 s (annex F)", P, 1000);
	AddCheck("0/8d",   "FIG 0/8",  "MCI, per data service component",             "at least every 1 s", P, 1000);
	AddCheck("0/9",    "FIG 0/9",  "SI, country, LTO and international table",    "once per second", P, 1000);
	AddCheck("0/10",   "FIG 0/10", "SI, date and time",                           "once per second", P, 1000);
	AddCheck("0/13",   "FIG 0/13", "MCI, user applications per component",        "at least every 1 s", P, 1000);
	AddCheck("0/14",   "FIG 0/14", "MCI, FEC per packet mode sub-channel",        "every 96 ms", P, 96);
	AddCheck("0/17",   "FIG 0/17", "SI, programme type per service",              "once per second", P, 1000);
	AddCheck("0/18",   "FIG 0/18", "SI, announcement support per service",        "once per second", P, 1000);
	AddCheck("0/19s",  "FIG 0/19", "Start of announcement",                       "10 times per second for 5 seconds", B, 100, 5000);
	AddCheck("0/19c",  "FIG 0/19", "Continuation of announcement",                "once per second", P, 1000);
	AddCheck("0/19e",  "FIG 0/19", "End of announcement",                         "10 times per second for 2 seconds", B, 100, 2000);
	AddCheck("0/20",   "FIG 0/20", "SI, service component information",           "once per second", P, 1000);
	AddCheck("0/21db", "FIG 0/21", "Long form, database definition",              "all information at least every 2 minutes", P, 120000);
	AddCheck("0/21cei","FIG 0/21", "Short form, CEI",                             "once per second for 5 seconds", B, 1000, 5000);
	AddCheck("0/24db", "FIG 0/24", "Long form, database definition",              "all information at least every 2 minutes", P, 120000);
	AddCheck("0/24cei","FIG 0/24", "Short form, CEI",                             "once per second for 5 seconds", B, 1000, 5000);
	AddCheck("0/25",   "FIG 0/25", "SI, OE announcement support",                 "once per second", P, 1000);
	AddCheck("0/26s",  "FIG 0/26", "Start of OE announcement",                    "10 times per second for 5 seconds", B, 100, 5000);
	AddCheck("0/26c",  "FIG 0/26", "Continuation of OE announcement",             "once per second", P, 1000);
	AddCheck("0/26e",  "FIG 0/26", "End of OE announcement",                      "10 times per second for 2 seconds", B, 100, 2000);
	AddCheck("1",      "FIG 1/x",  "SI, labels (FIG 1/0, 1/1, 1/4, 1/5, 1/6)",    "once per second", P, 1000);
	AddCheck("2",      "FIG 2/x",  "SI, extended labels",                         "once per second", P, 1000);
}

void FIGAnalyzer::Reset() {
	std::lock_guard<std::mutex> lock(mutex);
	now_ms = 0;
	frames = 0;
	fibs = 0;
	fib_crc_errors = 0;
	fig_bytes = 0;
	figs.clear();
	checks.clear();
	events.clear();
	open_cei.clear();
	clusters.clear();
	service_components.clear();
	InitChecks();
}

std::string FIGAnalyzer::FIGDescription(int type, int ext) {
	if(type == 0) {
		switch(ext) {
		case 0: return "Ensemble information";
		case 1: return "Basic sub-channel organization";
		case 2: return "Basic service and service component definition";
		case 3: return "Service component in packet mode";
		case 4: return "Service component with conditional access";
		case 5: return "Service component language";
		case 6: return "Service linking information";
		case 7: return "Configuration information";
		case 8: return "Service component global definition";
		case 9: return "Country, LTO and international table";
		case 10: return "Date and time";
		case 13: return "User application information";
		case 14: return "FEC sub-channel organization";
		case 17: return "Programme Type";
		case 18: return "Announcement support";
		case 19: return "Announcement switching";
		case 20: return "Service component information";
		case 21: return "Frequency information";
		case 24: return "OE Services";
		case 25: return "OE Announcement support";
		case 26: return "OE Announcement switching";
		}
	} else if(type == 1 || type == 2) {
		std::string extended = type == 2 ? "extended " : "";
		switch(ext) {
		case 0: return "Ensemble " + extended + "label";
		case 1: return "Programme service " + extended + "label";
		case 4: return "Service component " + extended + "label";
		case 5: return "Data service " + extended + "label";
		case 6: return "X-PAD user application " + extended + "label";
		}
	} else if(type == 5) {
		return "FIC data channel (FIDC)";
	} else if(type == 6) {
		return "Conditional access";
	}
	return "";
}

std::string FIGAnalyzer::FormatInterval(double ms) {
	char buf[32];
	if(ms < 1000)
		snprintf(buf, sizeof(buf), "%.0f ms", ms);
	else if(ms < 60000)
		snprintf(buf, sizeof(buf), "%.1f s", ms / 1000);
	else
		snprintf(buf, sizeof(buf), "%d:%02d min", (int) (ms / 60000), (int) (ms / 1000) % 60);
	return buf;
}

std::string FIGAnalyzer::AnnouncementTypeName(uint16_t asw) {
	if(!asw)
		return "none";
	std::string name;
	for(int type = 0; type < 16; type++)
		if(asw & (1 << type))
			name += (name.empty() ? "" : " + ") + FICDecoder::ConvertASuTypeToString(type);
	return name;
}


// --- feeding -----------------------------------------------------------------
void FIGAnalyzer::ProcessFIC(const uint8_t *data, size_t len) {
	std::lock_guard<std::mutex> lock(mutex);

	size_t fib_count = len / 32;
	for(size_t i = 0; i < fib_count; i++) {
		now_ms = frames * FRAME_MS + i * FRAME_MS / fib_count;
		ProcessFIB(data + i * 32);
	}
	frames++;
	now_ms = frames * FRAME_MS;

	CloseExpiredEvents();
}

void FIGAnalyzer::ProcessFIB(const uint8_t *fib) {
	fibs++;

	uint16_t crc_stored = fib[30] << 8 | fib[31];
	uint16_t crc_calced = CalcCRC::CalcCRC_CRC16_CCITT.Calc(fib, 30);
	if(crc_stored != crc_calced) {
		fib_crc_errors++;
		return;
	}

	for(size_t offset = 0; offset < 30 && fib[offset] != 0xFF;) {
		int type = fib[offset] >> 5;
		size_t len = fib[offset] & 0x1F;
		offset++;
		if(offset + len > 30)
			break;	// malformed

		fig_bytes += 1 + len;
		ProcessFIG(type, fib + offset, len);
		offset += len;
	}
}

void FIGAnalyzer::ProcessFIG(int type, const uint8_t *data, size_t len) {
	if(len < 1)
		return;

	// the FIG itself, whatever its content
	int ext = -1;
	if(type == 0)
		ext = data[0] & 0x1F;
	else if(type == 1 || type == 2)
		ext = data[0] & 0x07;
	std::string name = "FIG " + std::to_string(type) + (ext >= 0 ? "/" + std::to_string(ext) : "");
	Fig& fig = figs[name];
	if(fig.gaps.count == 0)
		fig.description = FIGDescription(type, ext);
	fig.gaps.Add(now_ms);
	fig.bytes += 1 + len;

	switch(type) {
	case 0:
		ProcessFIG0(data, len);
		break;
	case 1:
		ProcessFIG1(data, len);
		break;
	case 2:
		ProcessFIG2(data, len);
		break;
	}
}

void FIGAnalyzer::ProcessFIG0(const uint8_t *data, size_t len) {
	bool cn = data[0] & 0x80;
	bool oe = data[0] & 0x40;
	bool pd = data[0] & 0x20;
	int ext = data[0] & 0x1F;
	data++;
	len--;

	size_t sid_len = pd ? 4 : 2;
	auto read_sid = [&](size_t offset) -> uint32_t {
		return pd ? (uint32_t) data[offset] << 24 | data[offset + 1] << 16 | data[offset + 2] << 8 | data[offset + 3] :
				(uint32_t) (data[offset] << 8 | data[offset + 1]);
	};
	auto sid_name = [&](uint32_t sid) {return "SId " + Hex(sid, pd ? 8 : 4);};

	// FIG 0/6, 0/21, 0/24 use C/N and OE for their own purposes (database start/continuation,
	// other ensemble); 0/19 and 0/26 are temporary signalling - handled first
	switch(ext) {
	case 6:
		for(size_t offset = 0; offset + 2 <= len;) {
			bool id_list_flag = data[offset] & 0x80;
			bool sh = data[offset] & 0x20;
			bool ils = data[offset] & 0x10;
			int lsn = (data[offset] & 0x0F) << 8 | data[offset + 1];
			offset += 2;

			uint64_t key = Key(pd, (sh ? 2 : 0) | (ils ? 1 : 0), lsn);
			std::string name = "LSN " + Hex(lsn, 3) + (sh ? ", hard" : ", soft") + (ils ? ", international" : "") + (pd ? ", data" : "");

			if(!id_list_flag) {
				// short form: activation state (C/N = 1) or CEI (C/N = 0)
				if(cn)
					Occurrence("0/6act", key, name);
				else
					CEI("0/6cei", key, name);
				continue;
			}

			// long form: database start (C/N = 0) or continuation; both carry the activation state
			if(offset + 1 > len)
				break;
			int number_of_ids = data[offset] & 0x0F;
			offset++;
			offset += number_of_ids * (pd ? 4 : (ils ? 3 : 2));

			Occurrence("0/6act", key, name);
			if(!cn)
				Occurrence("0/6db", key, name);
		}
		return;
	case 21:
		for(size_t group_offset = 0; group_offset + 2 <= len;) {
			size_t length_of_fi_list = (data[group_offset] << 8 | data[group_offset + 1]) & 0x1F;
			group_offset += 2;
			size_t fi_list_end = std::min(group_offset + length_of_fi_list, len);

			for(size_t offset = group_offset; offset + 3 <= fi_list_end;) {
				uint16_t id = data[offset] << 8 | data[offset + 1];
				int rm = (data[offset + 2] >> 4) & 0x0F;
				size_t length_of_freq_list = data[offset + 2] & 0x07;
				offset += 3 + length_of_freq_list;

				static const char *ranges[16] = {"DAB", "?", "?", "?", "?", "?", "DRM", "?", "FM", "?", "?", "?", "?", "?", "AMSS", "?"};
				uint64_t key = Key(oe, id, rm);
				std::string name = std::string(rm == 8 ? "PI " : rm == 0 ? "EId " : "Id ") + Hex(id, 4) + " (" + ranges[rm] + ")" + (oe ? ", other ensemble/service" : "");

				if(length_of_freq_list == 0)
					CEI("0/21cei", key, name);
				else if(!cn)
					Occurrence("0/21db", key, name);
			}
			group_offset = fi_list_end;
		}
		return;
	case 24:
		for(size_t offset = 0; offset + sid_len + 1 <= len;) {
			uint32_t sid = read_sid(offset);
			offset += sid_len;
			int number_of_eids = data[offset] & 0x0F;
			offset += 1 + number_of_eids * 2;

			uint64_t key = Key(oe ? 2 | pd : pd, sid);
			std::string name = sid_name(sid) + (oe ? ", not in tuned ensemble" : "");

			if(number_of_eids == 0)
				CEI("0/24cei", key, name);
			else if(!cn)
				Occurrence("0/24db", key, name);
		}
		return;
	case 19:
		for(size_t offset = 0; offset + 4 <= len;) {
			uint8_t cid = data[offset];
			uint16_t asw = data[offset + 1] << 8 | data[offset + 2];
			bool region_flag = data[offset + 3] & 0x40;
			offset += region_flag ? 5 : 4;
			Announcement("0/19", Key(0, cid), "Cluster " + Hex(cid, 2), asw);
		}
		return;
	case 26:
		// OE announcement switching: only the first field's Cluster Id (current ensemble) and ASw
		// flags are used - enough to follow the announcement's signalling
		if(len >= 3) {
			uint8_t cid = data[0];
			uint16_t asw = data[1] << 8 | data[2];
			Announcement("0/26", Key(1, cid), "Cluster " + Hex(cid, 2), asw);
		}
		return;
	}

	// the remaining FIGs: only the current configuration of the tuned ensemble
	if(cn || oe)
		return;

	switch(ext) {
	case 0:
		Occurrence("0/0", 0, "Ensemble");
		break;
	case 1:
		for(size_t offset = 0; offset + 3 <= len;) {
			int subchid = data[offset] >> 2;
			bool long_form = data[offset + 2] & 0x80;
			offset += long_form ? 4 : 3;
			Occurrence("0/1", Key(0, subchid), "SubChId " + std::to_string(subchid));
		}
		break;
	case 2: {
		for(size_t offset = 0; offset + sid_len + 1 <= len;) {
			uint32_t sid = read_sid(offset);
			offset += sid_len;
			int number_of_comps = data[offset] & 0x0F;
			offset += 1 + number_of_comps * 2;
			Occurrence("0/2", Key(pd, sid), sid_name(sid));
			service_components[sid] = number_of_comps;
		}
		break; }
	case 3:
		for(size_t offset = 0; offset + 5 <= len;) {
			int scid = data[offset] << 4 | data[offset + 1] >> 4;
			bool scca_flag = data[offset + 1] & 0x01;
			offset += scca_flag ? 7 : 5;
			Occurrence("0/3", Key(0, scid), "SCId " + std::to_string(scid));
		}
		break;
	case 5:
		for(size_t offset = 0; offset < len;) {
			bool ls_flag = data[offset] & 0x80;
			if(ls_flag) {
				if(offset + 3 > len)
					break;
				int scid = (data[offset] & 0x0F) << 8 | data[offset + 1];
				Occurrence("0/5", Key(2, scid), "SCId " + std::to_string(scid));
				offset += 3;
			} else {
				if(offset + 2 > len)
					break;
				bool msc_fic = data[offset] & 0x40;
				int id = data[offset] & 0x3F;
				Occurrence("0/5", Key(msc_fic, id), (msc_fic ? "FIDCId " : "SubChId ") + std::to_string(id));
				offset += 2;
			}
		}
		break;
	case 7:
		Occurrence("0/7", 0, "Ensemble");
		break;
	case 8:
		for(size_t offset = 0; offset + sid_len + 2 <= len;) {
			uint32_t sid = read_sid(offset);
			offset += sid_len;
			bool ext_flag = data[offset] & 0x80;
			int scids = data[offset] & 0x0F;
			offset++;
			bool ls_flag = data[offset] & 0x80;
			offset += ls_flag ? 2 : 1;
			if(ext_flag)
				offset++;
			Occurrence(pd ? "0/8d" : "0/8p", Key(0, sid, scids), sid_name(sid) + ", SCIdS " + std::to_string(scids));
		}
		break;
	case 9:
		Occurrence("0/9", 0, "Ensemble");
		break;
	case 10:
		Occurrence("0/10", 0, "Date and time");
		break;
	case 13:
		for(size_t offset = 0; offset + sid_len + 1 <= len;) {
			uint32_t sid = read_sid(offset);
			offset += sid_len;
			int scids = data[offset] >> 4;
			int number_of_uas = data[offset] & 0x0F;
			offset++;
			for(int i = 0; i < number_of_uas && offset + 2 <= len; i++)
				offset += 2 + (data[offset + 1] & 0x1F);
			Occurrence("0/13", Key(pd, sid, scids), sid_name(sid) + ", SCIdS " + std::to_string(scids));
		}
		break;
	case 14:
		for(size_t offset = 0; offset < len; offset++) {
			int subchid = data[offset] >> 2;
			Occurrence("0/14", Key(0, subchid), "SubChId " + std::to_string(subchid));
		}
		break;
	case 17:
		for(size_t offset = 0; offset + 4 <= len;) {
			uint32_t sid = data[offset] << 8 | data[offset + 1];
			bool l_flag = data[offset + 2] & 0x20;
			bool cc_flag = data[offset + 2] & 0x10;
			offset += 3 + (l_flag ? 1 : 0) + 1 + (cc_flag ? 1 : 0);
			Occurrence("0/17", Key(0, sid), "SId " + Hex(sid, 4));
		}
		break;
	case 18:
		for(size_t offset = 0; offset + 5 <= len;) {
			uint32_t sid = data[offset] << 8 | data[offset + 1];
			size_t number_of_clusters = data[offset + 4] & 0x1F;
			offset += 5 + number_of_clusters;
			Occurrence("0/18", Key(0, sid), "SId " + Hex(sid, 4));
		}
		break;
	case 20:
		Occurrence("0/20", 0, "(any)");
		break;
	case 25:
		Occurrence("0/25", 0, "(any)");
		break;
	}
}

void FIGAnalyzer::ProcessFIG1(const uint8_t *data, size_t len) {
	bool oe = data[0] & 0x08;
	int ext = data[0] & 0x07;
	data++;
	len--;

	// labels of other ensembles aren't covered by the once-per-second rate
	if(oe)
		return;

	std::string name;
	uint64_t key = 0;
	switch(ext) {
	case 0:
		if(len < 2)
			return;
		key = Key(0, data[0] << 8 | data[1]);
		name = "Ensemble label (EId " + Hex(data[0] << 8 | data[1], 4) + ")";
		break;
	case 1:
		if(len < 2)
			return;
		key = Key(1, data[0] << 8 | data[1]);
		name = "Service label, SId " + Hex(data[0] << 8 | data[1], 4);
		break;
	case 4: {
		if(len < 3)
			return;
		bool pd = data[0] & 0x80;
		int scids = data[0] & 0x0F;
		uint32_t sid = pd ? (len >= 5 ? (uint32_t) data[1] << 24 | data[2] << 16 | data[3] << 8 | data[4] : 0) : (uint32_t) (data[1] << 8 | data[2]);
		key = Key(4, sid, scids);
		name = "Component label, SId " + Hex(sid, pd ? 8 : 4) + ", SCIdS " + std::to_string(scids);
		break; }
	case 5: {
		if(len < 4)
			return;
		uint32_t sid = (uint32_t) data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
		key = Key(5, sid);
		name = "Data service label, SId " + Hex(sid, 8);
		break; }
	default:
		key = Key(ext);
		name = "FIG 1/" + std::to_string(ext) + " labels";
		break;
	}
	Occurrence("1", key, name);
}

void FIGAnalyzer::ProcessFIG2(const uint8_t *data, size_t len) {
	int segment = (data[0] >> 4) & 0x07;
	int ext = data[0] & 0x07;
	data++;
	len--;

	uint32_t id = 0;
	int id_nibbles = 4;
	if(ext == 5) {
		if(len < 4)
			return;
		id = (uint32_t) data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
		id_nibbles = 8;
	} else if(ext == 4) {
		if(len < 3)
			return;
		id = (uint32_t) (data[1] << 8 | data[2]) << 4 | (data[0] & 0x0F);
		id_nibbles = 5;
	} else {
		if(len < 2)
			return;
		id = data[0] << 8 | data[1];
	}
	// each segment of a label is an entity: all of them have to come round once per second
	Occurrence("2", Key(ext, id, segment), "FIG 2/" + std::to_string(ext) + " " + Hex(id, id_nibbles) + ", segment " + std::to_string(segment));
}


// --- checks and events -----------------------------------------------------------------
void FIGAnalyzer::Occurrence(const std::string& check_id, uint64_t key, const std::string& name) {
	std::map<std::string, Check>::iterator it = checks.find(check_id);
	if(it == checks.end())
		return;
	Entity& entity = it->second.entities[key];
	if(entity.gaps.count == 0)
		entity.name = name;
	else
		entity.histogram.Add(now_ms - entity.gaps.last_ms, it->second.nominal_ms);
	entity.gaps.Add(now_ms);
}

FIGAnalyzer::Event& FIGAnalyzer::OpenEvent(const std::string& check_id, const std::string& entity, const std::string& event, double window_ms, double nominal_ms) {
	Check& check = checks[check_id];
	check.events++;

	Event e;
	e.time = time(nullptr);
	e.t0_ms = now_ms;
	e.check_id = check_id;
	e.fig = check.fig;
	e.entity = entity;
	e.event = event;
	e.requirement = check.requirement;
	e.nominal_ms = nominal_ms;
	e.window_ms = window_ms;
	e.complete = false;
	// signalling already in progress when the observation started can't be judged as a burst
	if(now_ms < 1500)
		e.note = "already in progress when the observation started";
	events.push_back(e);

	while(events.size() > MAX_EVENTS && events.front().complete)
		events.pop_front();
	return events.back();
}

void FIGAnalyzer::CEI(const std::string& check_id, uint64_t key, const std::string& name) {
	const Check& check = checks[check_id];
	uint64_t event_key = std::hash<std::string>()(check_id) ^ key;

	std::map<uint64_t, Event*>::iterator it = open_cei.find(event_key);
	Event *event = it != open_cei.end() ? it->second : nullptr;
	if(!event) {
		event = &OpenEvent(check_id, name, "CEI (change event indication)", check.burst_window_ms, check.nominal_ms);
		open_cei[event_key] = event;
	}
	AddToEvent(*event);
}

void FIGAnalyzer::Announcement(const std::string& fig, uint64_t key, const std::string& name, uint16_t asw) {
	Cluster& cluster = clusters[std::hash<std::string>()(fig) ^ key];

	if(asw) {
		if(!cluster.active || cluster.asw != asw) {
			// start of an announcement (the ASw flags shall not change during one, other than to end it)
			cluster.active = true;
			cluster.asw = asw;
			cluster.start_ms = now_ms;
			cluster.start_event = &OpenEvent(fig + "s", name, "Announcement start (" + AnnouncementTypeName(asw) + ")", 5000, 100);
			cluster.end_event = nullptr;
			// the continuation is measured from scratch for each announcement
			checks[fig + "c"].entities.erase(key);
		}

		if(cluster.start_event && !cluster.start_event->complete) {
			AddToEvent(*cluster.start_event);
		} else {
			cluster.start_event = nullptr;
			Occurrence(fig + "c", key, name + " (" + AnnouncementTypeName(asw) + ")");
		}
	} else {
		if(cluster.active) {
			// end of the announcement
			cluster.active = false;
			cluster.start_event = nullptr;
			cluster.end_event = &OpenEvent(fig + "e", name, "Announcement end (" + AnnouncementTypeName(cluster.asw) + ")", 2000, 100);
			checks[fig + "c"].entities.erase(key);
		}
		if(cluster.end_event && !cluster.end_event->complete)
			AddToEvent(*cluster.end_event);
	}
}

void FIGAnalyzer::AddToEvent(Event& event) {
	if(event.gaps.count)
		event.histogram.Add(now_ms - event.gaps.last_ms, event.nominal_ms);
	event.gaps.Add(now_ms);
}

void FIGAnalyzer::CloseExpiredEvents() {
	// a burst is judged once its window (plus one nominal interval of grace) is over
	for(Event& event : events) {
		if(event.complete || now_ms < event.t0_ms + event.window_ms + event.nominal_ms)
			continue;
		event.complete = true;
	}
	for(std::map<uint64_t, Event*>::iterator it = open_cei.begin(); it != open_cei.end();) {
		if(it->second->complete)
			it = open_cei.erase(it);
		else
			++it;
	}
	for(std::pair<const uint64_t, Cluster>& kv : clusters) {
		if(kv.second.start_event && kv.second.start_event->complete)
			kv.second.start_event = nullptr;
		if(kv.second.end_event && kv.second.end_event->complete)
			kv.second.end_event = nullptr;
	}
}


// --- verdicts and snapshot -----------------------------------------------------------------
FIGVerdict FIGAnalyzer::PeriodicVerdict(double max_gap_ms, bool has_closed_gap, double nominal_ms) {
	// nothing to judge yet: only one occurrence so far, and not overdue
	if(!has_closed_gap && max_gap_ms <= nominal_ms + TOLERANCE_MS)
		return FIGVerdict::OBSERVING;
	if(max_gap_ms <= nominal_ms + TOLERANCE_MS)
		return FIGVerdict::OK;
	// "shall not fall below one third of the nominal rates"
	if(max_gap_ms <= 3 * nominal_ms + TOLERANCE_MS)
		return FIGVerdict::DEGRADED;
	return FIGVerdict::FAIL;
}

FIGVerdict FIGAnalyzer::EventVerdict(const Event& event) {
	if(!event.note.empty())
		return FIGVerdict::NONE;

	// the nominal number of repetitions over the window, e.g. 5 for "once per second for 5 seconds"
	double expected = event.window_ms / event.nominal_ms;
	int repetitions = (int) event.gaps.count;
	double max_gap = event.gaps.max_gap_ms;

	FIGVerdict gap_verdict;
	if(repetitions < 2)
		gap_verdict = event.complete ? FIGVerdict::FAIL : FIGVerdict::OBSERVING;
	else if(max_gap <= event.nominal_ms + TOLERANCE_MS)
		gap_verdict = FIGVerdict::OK;
	else if(max_gap <= 3 * event.nominal_ms + TOLERANCE_MS)
		gap_verdict = FIGVerdict::DEGRADED;
	else
		gap_verdict = FIGVerdict::FAIL;

	if(!event.complete)
		return gap_verdict == FIGVerdict::FAIL ? FIGVerdict::FAIL : FIGVerdict::OBSERVING;

	FIGVerdict count_verdict;
	if(repetitions + 0.5 >= expected)
		count_verdict = FIGVerdict::OK;
	else if(repetitions + 0.5 >= expected / 3)
		count_verdict = FIGVerdict::DEGRADED;
	else
		count_verdict = FIGVerdict::FAIL;

	return std::max(gap_verdict, count_verdict);
}

FIG_ANALYSIS_SNAPSHOT FIGAnalyzer::Snapshot() {
	std::lock_guard<std::mutex> lock(mutex);

	FIG_ANALYSIS_SNAPSHOT snapshot;
	snapshot.observed_ms = now_ms;
	snapshot.fibs = fibs;
	snapshot.fib_crc_errors = fib_crc_errors;
	uint64_t good_fibs = fibs - fib_crc_errors;
	snapshot.fic_load = good_fibs ? (double) fig_bytes / (good_fibs * 30) : 0;
	snapshot.services = (int) service_components.size();
	for(const std::pair<const uint32_t, int>& kv : service_components)
		snapshot.components += kv.second;

	std::map<std::string, FIGVerdict> fig_verdicts;

	// in definition order (FIG 0/0, 0/1, ... - not alphabetical)
	std::vector<const std::pair<const std::string, Check>*> ordered;
	for(const std::pair<const std::string, Check>& kv : checks)
		ordered.push_back(&kv);
	std::sort(ordered.begin(), ordered.end(), [](const std::pair<const std::string, Check> *a, const std::pair<const std::string, Check> *b) {
		return a->second.order < b->second.order;
	});

	for(const std::pair<const std::string, Check> *kv_ptr : ordered) {
		const std::pair<const std::string, Check>& kv = *kv_ptr;
		const Check& check = kv.second;

		FIG_ANALYSIS_CHECK c;
		c.fig = check.fig;
		c.usage = check.usage;
		c.requirement = check.requirement;
		c.nominal_ms = check.nominal_ms;
		c.worst_gap_ms = 0;
		c.burst = check.kind == CheckKind::BURST;
		c.events = check.events;
		c.verdict = FIGVerdict::NONE;

		if(check.kind == CheckKind::PERIODIC) {
			for(const std::pair<const uint64_t, Entity>& ekv : check.entities) {
				const Entity& entity = ekv.second;
				FIG_ANALYSIS_ENTITY e;
				e.name = entity.name;
				e.count = entity.gaps.count;
				e.since_last_ms = now_ms - entity.gaps.last_ms;
				e.min_gap_ms = entity.gaps.min_gap_ms;
				e.avg_gap_ms = entity.gaps.AvgGap();
				e.max_gap_ms = std::max(entity.gaps.max_gap_ms, e.since_last_ms);
				// for the slowly repeated databases (10 s, 2 min), the wait from the start of the
				// observation until an entity's first occurrence counts too - it is at least that long
				if(check.nominal_ms >= 10000)
					e.max_gap_ms = std::max(e.max_gap_ms, entity.gaps.first_ms);
				e.verdict = PeriodicVerdict(e.max_gap_ms, entity.gaps.count > 1, check.nominal_ms);
				e.histogram = entity.histogram;
				c.histogram.Merge(entity.histogram);
				c.worst_gap_ms = std::max(c.worst_gap_ms, e.max_gap_ms);
				c.verdict = c.verdict == FIGVerdict::NONE ? e.verdict : std::max(c.verdict, e.verdict);
				c.entities.push_back(e);
			}
		} else {
			for(const Event& event : events) {
				if(event.check_id != kv.first)
					continue;
				FIGVerdict v = EventVerdict(event);
				c.histogram.Merge(event.histogram);
				if(v != FIGVerdict::NONE)
					c.verdict = c.verdict == FIGVerdict::NONE ? v : std::max(c.verdict, v);
				c.worst_gap_ms = std::max(c.worst_gap_ms, event.gaps.max_gap_ms);
			}
		}

		// only checks that have seen something are of interest
		if(c.entities.empty() && c.events == 0)
			continue;

		FIGVerdict& fv = fig_verdicts[c.fig];
		if(c.verdict != FIGVerdict::NONE)
			fv = fv == FIGVerdict::NONE ? c.verdict : std::max(fv, c.verdict);
		snapshot.checks.push_back(c);
	}

	for(const std::pair<const std::string, Fig>& kv : figs) {
		FIG_ANALYSIS_FIG f;
		f.fig = kv.first;
		f.description = kv.second.description;
		f.count = kv.second.gaps.count;
		f.bytes = kv.second.bytes;
		f.rate_per_s = now_ms > 0 ? f.count * 1000.0 / now_ms : 0;
		// including the wait for its very first occurrence
		f.max_gap_ms = std::max({kv.second.gaps.max_gap_ms, now_ms - kv.second.gaps.last_ms, kv.second.gaps.first_ms});

		// FIG 1/x and 2/x share one check each
		std::string check_fig = f.fig;
		if(check_fig.compare(0, 6, "FIG 1/") == 0)
			check_fig = "FIG 1/x";
		else if(check_fig.compare(0, 6, "FIG 2/") == 0)
			check_fig = "FIG 2/x";
		std::map<std::string, FIGVerdict>::const_iterator vit = fig_verdicts.find(check_fig);
		f.verdict = vit != fig_verdicts.end() ? vit->second : FIGVerdict::NONE;
		snapshot.figs.push_back(f);
	}
	// FIG 0/x in numerical order, then FIG 1/x, ...
	std::sort(snapshot.figs.begin(), snapshot.figs.end(), [](const FIG_ANALYSIS_FIG& a, const FIG_ANALYSIS_FIG& b) {
		int ta, ea = -1, tb, eb = -1;
		sscanf(a.fig.c_str(), "FIG %d/%d", &ta, &ea);
		sscanf(b.fig.c_str(), "FIG %d/%d", &tb, &eb);
		return ta != tb ? ta < tb : ea < eb;
	});

	for(std::deque<Event>::const_reverse_iterator it = events.rbegin(); it != events.rend(); ++it) {
		FIG_ANALYSIS_EVENT e;
		e.time = it->time;
		e.t_ms = it->t0_ms;
		e.fig = it->fig;
		e.entity = it->entity;
		e.event = it->event;
		e.requirement = it->requirement;
		e.repetitions = (int) it->gaps.count;
		e.span_ms = it->gaps.count ? it->gaps.last_ms - it->gaps.first_ms : 0;
		e.max_gap_ms = it->gaps.max_gap_ms;
		e.complete = it->complete;
		e.verdict = EventVerdict(*it);
		e.note = it->note;
		e.nominal_ms = it->nominal_ms;
		e.histogram = it->histogram;
		snapshot.events.push_back(e);
	}

	return snapshot;
}
