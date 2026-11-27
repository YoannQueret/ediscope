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

#ifndef MNSC_DECODER_H_
#define MNSC_DECODER_H_

#include <stdint.h>
#include <mutex>
#include <vector>


// --- MNSC_TSC -----------------------------------------------------------------
// Transmitter Set-up and Control information (ETS 300 799 annex A.3.2.1, ASS message type 0): the
// values the multiplexer signals to one transmitter of the network, identified by its address
struct MNSC_TSC {
	uint16_t address;
	uint16_t control_flags;		// user defined
	int offset_delay_us;		// 0..2047
	int tii_main_id;			// 0..127
	int tii_sub_id;				// 0..31

	MNSC_TSC() : address(0), control_flags(0), offset_delay_us(0), tii_main_id(0), tii_sub_id(0) {}
};


// --- MNSCDecoderObserver -----------------------------------------------------------------
class MNSCDecoderObserver {
public:
	virtual ~MNSCDecoderObserver() {}

	// a complete TSC message, with a correct CRC
	virtual void MNSCTSCMessage(const MNSC_TSC& /*tsc*/) {}
	// a complete ASS message whose CRC is wrong (discarded)
	virtual void MNSCCRCError() {}
};


// --- MNSCDecoder -----------------------------------------------------------------
// the Multiplex Network Signalling Channel (ETS 300 799 clause 5.5.1 and annex A): the byte pairs
// of four consecutive frames (FP 0..3, then 4..7) form an 8-byte signalling group; asynchronous
// (ASS) messages are a start group, GrNb continuation groups and an end group with a CRC, possibly
// with padding groups in between. Only the TSC messages (ASS type 0) are reported
class MNSCDecoder {
private:
	MNSCDecoderObserver *observer;

	// Process() runs on the player thread, Reset() on the UI thread
	std::mutex mutex;

	uint8_t group[8];
	int group_pos;		// number of byte pairs in group, -1 while waiting for the next group start

	bool in_message;
	int continuations_expected;
	int continuations;
	std::vector<uint8_t> message;	// from the start group's first byte to the end group's last message byte

	void ProcessGroup();
public:
	MNSCDecoder(MNSCDecoderObserver *observer) : observer(observer), group_pos(-1), in_message(false), continuations_expected(0), continuations(0) {}

	void Process(int fp, const uint8_t *mnsc);
	void Reset();
};

#endif /* MNSC_DECODER_H_ */
