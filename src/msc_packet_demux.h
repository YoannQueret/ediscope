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

#ifndef MSC_PACKET_DEMUX_H_
#define MSC_PACKET_DEMUX_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "subchannel_sink.h"


// --- MSCPacketDemuxObserver -----------------------------------------------------------------
class MSCPacketDemuxObserver {
public:
	virtual ~MSCPacketDemuxObserver() {}

	// one complete MSC data group (ETSI EN 300 401 clause 5.3.3, figure 12: data group header +
	// optional session header + data field + CRC, exactly as transmitted) reassembled from a run
	// of packets sharing First/Last flags - still fully unparsed at this level
	virtual void MSCDataGroupReceived(const uint8_t *data, size_t len) = 0;
};


// --- MSCPacketDemux -----------------------------------------------------------------
// De-multiplexes ETSI EN 300 401 clause 5.3.2 MSC packet mode: extracts the packets carrying a
// single configured packet Address out of a (possibly shared) data sub-channel's raw byte stream,
// and reassembles the MSC data groups (clause 5.3.3) spread across them.
//
// The outer Reed-Solomon FEC scheme (clause 5.3.5), if applied by the mux, is intentionally NOT
// undone here: it exists purely to correct RF transmission errors, which cannot occur on the
// clean, already error-checked ETI-NI/EDI transport ediscope always consumes (never raw IQ). A
// receiver without FEC capability discards the FEC packets for free, simply because they carry a
// reserved Address (1022) that never matches any real service's configured address (see clause
// 5.3.5.2's own note on this) - the same thing naturally happens here.
class MSCPacketDemux : public SubchannelSink {
private:
	int address;
	std::vector<uint8_t> buffer;

	std::vector<uint8_t> dg_buffer;
	bool dg_in_progress;

	MSCPacketDemuxObserver *dg_observer;

	void ProcessPacketPayload(const uint8_t *data, size_t len, bool first, bool last);
public:
	MSCPacketDemux(int address, MSCPacketDemuxObserver *dg_observer);

	void Feed(const uint8_t *data, size_t len) override;
};

#endif /* MSC_PACKET_DEMUX_H_ */
