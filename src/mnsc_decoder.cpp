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

#include "mnsc_decoder.h"

#include "tools.h"


// --- MNSCDecoder -----------------------------------------------------------------
void MNSCDecoder::Reset() {
	std::lock_guard<std::mutex> lock(mutex);
	group_pos = -1;
	in_message = false;
	message.clear();
}

void MNSCDecoder::Process(int fp, const uint8_t *mnsc) {
	std::lock_guard<std::mutex> lock(mutex);

	// the two least significant FP bits give the byte pair index within the group (table 7) - a
	// group is only used if its four byte pairs arrived in sequence
	int index = fp & 0x03;
	if(index == 0)
		group_pos = 0;
	else if(group_pos != index)
		group_pos = -1;
	if(group_pos == -1)
		return;

	group[index * 2] = mnsc[0];
	group[index * 2 + 1] = mnsc[1];
	group_pos++;

	if(group_pos == 4) {
		ProcessGroup();
		group_pos = -1;
	}
}

void MNSCDecoder::ProcessGroup() {
	// SB0 b0,b1 (MSBs): 00 = frame synchronous (FSS), 11 = asynchronous (ASS); FSS groups are
	// single-group messages, not handled here
	if((group[0] >> 6) != 0x03)
		return;

	switch(group[0] & 0x0F) {
	case 0x0:	// start group: message type (SB2), GrNb (SB3)
		in_message = true;
		continuations_expected = group[3];
		continuations = 0;
		message.assign(group, group + 8);
		break;
	case 0x3:	// continuation group
		if(!in_message || continuations >= continuations_expected) {
			in_message = false;
			break;
		}
		message.insert(message.end(), group, group + 8);
		continuations++;
		break;
	case 0xC: {	// end group: CRC over the message, from the start group's SB0 to this group's SB5
		if(!in_message || continuations != continuations_expected) {
			in_message = false;
			break;
		}
		in_message = false;
		message.insert(message.end(), group, group + 6);

		uint16_t crc_stored = group[6] << 8 | group[7];
		uint16_t crc_calced = CalcCRC::CalcCRC_CRC16_CCITT.Calc(message.data(), message.size());
		if(crc_stored != crc_calced) {
			observer->MNSCCRCError();
			break;
		}

		// TSC (type 0): address and control flags in the start group, offset delay and TII in the end group
		if(message[2] != 0x00)
			break;
		MNSC_TSC tsc;
		tsc.address = message[4] << 8 | message[5];
		tsc.control_flags = message[6] << 8 | message[7];
		tsc.offset_delay_us = (group[2] & 0x07) << 8 | group[3];
		tsc.tii_main_id = group[4] & 0x7F;
		tsc.tii_sub_id = group[5] & 0x1F;
		observer->MNSCTSCMessage(tsc);
		break; }
	case 0xF:	// padding group - not part of the message
		break;
	default:	// Rfa
		in_message = false;
	}
}
