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

#include "msc_packet_demux.h"

#include <cstdio>

#include "tools.h"


// --- MSCPacketDemux -----------------------------------------------------------------
MSCPacketDemux::MSCPacketDemux(int address, MSCPacketDemuxObserver *dg_observer) :
		SubchannelSink(nullptr, ""),
		address(address),
		dg_in_progress(false),
		dg_observer(dg_observer)
{}

void MSCPacketDemux::Feed(const uint8_t *data, size_t len) {
	// ETSI EN 300 401 clause 5.3.2.1: packet lengths/data field lengths, indexed by the 2-bit
	// "Packet length" field
	static const size_t packet_lengths[4] = {24, 48, 72, 96};
	static const size_t data_field_lengths[4] = {19, 43, 67, 91};

	buffer.insert(buffer.end(), data, data + len);

	size_t pos = 0;
	while(pos + 3 <= buffer.size()) {
		int length_code = buffer[pos] >> 6;
		size_t packet_len = packet_lengths[length_code];
		if(pos + packet_len > buffer.size())
			break;	// wait for the rest - shouldn't normally happen, as packets don't span frames

		int continuity_index = (buffer[pos] >> 4) & 0x03;
		bool first = (buffer[pos] >> 3) & 0x01;
		bool last = (buffer[pos] >> 2) & 0x01;
		int addr = (buffer[pos] & 0x03) << 8 | buffer[pos + 1];
		bool command = (buffer[pos + 2] >> 7) & 0x01;
		size_t useful_len = buffer[pos + 2] & 0x7F;
		(void) continuity_index;	// not currently cross-checked - see ProcessPacketPayload's resync-on-anomaly handling instead

		if(addr == address && !command) {
			uint16_t crc_stored = buffer[pos + packet_len - 2] << 8 | buffer[pos + packet_len - 1];
			uint16_t crc_calced = CalcCRC::CalcCRC_CRC16_CCITT.Calc(&buffer[pos], packet_len - 2);
			if(crc_stored == crc_calced) {
				size_t data_field_len = data_field_lengths[length_code];
				if(useful_len > data_field_len)
					useful_len = data_field_len;	// malformed - clamp rather than read past the data field
				ProcessPacketPayload(&buffer[pos + 3], useful_len, first, last);
			} else {
				fprintf(stderr, "MSCPacketDemux: dropped packet (address %d) with bad CRC\n", address);
				dg_buffer.clear();
				dg_in_progress = false;
			}
		}

		pos += packet_len;
	}

	buffer.erase(buffer.begin(), buffer.begin() + pos);
}

void MSCPacketDemux::ProcessPacketPayload(const uint8_t *data, size_t len, bool first, bool last) {
	if(first) {
		if(dg_in_progress)
			fprintf(stderr, "MSCPacketDemux: new data group started before the previous one completed - discarding it\n");
		dg_buffer.clear();
		dg_in_progress = true;
	}

	if(!dg_in_progress)
		return;	// joined mid-group (e.g. right after startup) - wait for the next 'first' packet

	dg_buffer.insert(dg_buffer.end(), data, data + len);

	if(last) {
		dg_observer->MSCDataGroupReceived(dg_buffer.data(), dg_buffer.size());
		dg_buffer.clear();
		dg_in_progress = false;
	}
}
