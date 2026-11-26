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

#include "edi_to_eti.h"

#include <cstring>
#include <map>
#include <string>

#include "tools.h"


// --- EDIToETI -----------------------------------------------------------------
bool EDIToETI::Convert(const uint8_t *af, std::vector<uint8_t>& eti_frame) {
	// AF packet: "AF", LEN (4), SEQ (2), AR (1: CF, MAJ, MIN), PT (1: 'T'), payload, CRC (2)
	if(af[0] != 'A' || af[1] != 'F' || af[9] != 'T')
		return false;
	size_t len = (size_t) af[2] << 24 | af[3] << 16 | af[4] << 8 | af[5];
	if(af[8] & 0x80) {
		uint16_t crc_stored = af[10 + len] << 8 | af[10 + len + 1];
		if(crc_stored != CalcCRC::CalcCRC_CRC16_CCITT.Calc(af, 10 + len))
			return false;
	}

	// the deti TAG item and the sub-channel streams (est<n>), in stream order
	const uint8_t *deti = nullptr;
	size_t deti_len = 0;
	struct Stream {const uint8_t *value; size_t len;};
	std::map<int, Stream> streams;

	size_t tag_item_len_bytes;
	for(size_t i = 0; i + 8 <= len; i += tag_item_len_bytes) {
		const uint8_t *tag_item = af + 10 + i;
		std::string tag_name((const char*) tag_item, 4);
		size_t tag_len = (size_t) tag_item[4] << 24 | tag_item[5] << 16 | tag_item[6] << 8 | tag_item[7];
		tag_item_len_bytes = 8 + (tag_len + 7) / 8;
		if(i + tag_item_len_bytes > len)
			return false;	// malformed

		if(tag_name == "deti") {
			deti = tag_item + 8;
			deti_len = tag_len / 8;
		} else if(tag_name.compare(0, 3, "est") == 0 && tag_item[3] >= 1 && tag_item[3] <= 64 && tag_len >= 3 * 8) {
			streams[tag_item[3]] = Stream{tag_item + 8, tag_len / 8};
		}
	}
	if(!deti || deti_len < 6)
		return false;

	// deti: ATSTF, FICF, RFUDF, FCTH (1); FCT (1); STAT (1); MID, FP, Rfa (1); MNSC (2);
	// [ATST (8): UTCO, seconds, TSTA]; [FIC]; [RFUD (3)]
	bool atstf = deti[0] & 0x80;
	bool ficf = deti[0] & 0x40;
	int fct = deti[1];
	uint8_t stat = deti[2];
	int mid = deti[3] >> 6;
	int fp = (deti[3] >> 3) & 0x07;
	const uint8_t *mnsc = deti + 4;
	size_t offset = 6;
	uint32_t tsta = 0xFFFFFF;
	if(atstf) {
		if(deti_len < offset + 8)
			return false;
		tsta = (uint32_t) deti[offset + 5] << 16 | deti[offset + 6] << 8 | deti[offset + 7];
		offset += 8;
	}
	size_t fic_len = ficf ? (mid == 3 ? 128 : 96) : 0;
	if(deti_len < offset + fic_len)
		return false;
	const uint8_t *fic = deti + offset;

	size_t nst = streams.size();
	size_t mst_len = fic_len;
	for(const std::pair<const int, Stream>& kv : streams)
		mst_len += kv.second.len - 3;

	// SYNC (4) + FC (4) + STC (4 * NST) + EOH (4) + MST + EOF (4) + TIST (4)
	if(8 + 4 * nst + 4 + mst_len + 8 > eti_frame_size)
		return false;

	eti_frame.assign(eti_frame_size, 0x55);	// the padding
	uint8_t *eti = &eti_frame[0];

	// SYNC: ERR (the EDI STAT carries the same error level), FSYNC alternating from frame to frame
	eti[0] = stat;
	uint32_t fsync = fct % 2 ? 0xF8C549 : 0x073AB6;
	eti[1] = fsync >> 16;
	eti[2] = fsync >> 8;
	eti[3] = fsync;

	// FC: FCT; FICF, NST; FP, MID, FL - FL being the number of 32 bit words of STC, EOH and MST
	size_t fl = nst + 1 + mst_len / 4;
	eti[4] = fct;
	eti[5] = (ficf ? 0x80 : 0x00) | nst;
	eti[6] = fp << 5 | mid << 3 | (fl >> 8 & 0x07);
	eti[7] = fl;

	// STC: SCID, SAD, TPL, STL (in 64 bit units) per stream
	size_t pos = 8;
	for(const std::pair<const int, Stream>& kv : streams) {
		const uint8_t *est = kv.second.value;
		int scid = est[0] >> 2;
		int sad = (est[0] & 0x03) << 8 | est[1];
		int tpl = est[2] >> 2;
		size_t stl = (kv.second.len - 3) / 8;
		eti[pos++] = scid << 2 | sad >> 8;
		eti[pos++] = sad;
		eti[pos++] = tpl << 2 | (stl >> 8 & 0x03);
		eti[pos++] = stl;
	}

	// EOH: MNSC, CRC over FC, STC and MNSC
	eti[pos++] = mnsc[0];
	eti[pos++] = mnsc[1];
	uint16_t header_crc = CalcCRC::CalcCRC_CRC16_CCITT.Calc(eti + 4, pos - 4);
	eti[pos++] = header_crc >> 8;
	eti[pos++] = header_crc;

	// MST: FIC, then the sub-channel streams
	size_t mst_start = pos;
	if(fic_len) {
		memcpy(eti + pos, fic, fic_len);
		pos += fic_len;
	}
	for(const std::pair<const int, Stream>& kv : streams) {
		memcpy(eti + pos, kv.second.value + 3, kv.second.len - 3);
		pos += kv.second.len - 3;
	}

	// EOF: CRC over MST, RFU
	uint16_t mst_crc = CalcCRC::CalcCRC_CRC16_CCITT.Calc(eti + mst_start, pos - mst_start);
	eti[pos++] = mst_crc >> 8;
	eti[pos++] = mst_crc;
	eti[pos++] = 0xFF;
	eti[pos++] = 0xFF;

	// TIST: TSTA (in 1/16384 ms) followed by 0xFF - all ones if the frame carries no time stamp
	uint32_t tist = tsta << 8 | 0xFF;
	eti[pos++] = tist >> 24;
	eti[pos++] = tist >> 16;
	eti[pos++] = tist >> 8;
	eti[pos++] = tist;

	return true;
}
