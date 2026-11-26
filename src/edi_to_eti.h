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

#ifndef EDI_TO_ETI_H_
#define EDI_TO_ETI_H_

#include <cstddef>
#include <cstdint>
#include <vector>


// --- EDIToETI -----------------------------------------------------------------
// Converts one EDI AF packet carrying a DAB ETI(LI) frame (ETSI TS 102 693: a "deti" TAG item
// plus one "est<n>" TAG item per sub-channel) into the equivalent ETI(NI) frame (ETSI EN 300 799:
// SYNC, frame characterization, stream characterization, end of header, main stream - FIC and
// sub-channels -, end of frame, time stamp, padded to 6144 bytes)
class EDIToETI {
public:
	static const size_t eti_frame_size = 6144;

	// returns false (and leaves eti_frame untouched) if the packet isn't a valid ETI-carrying AF packet
	static bool Convert(const uint8_t *af_packet, std::vector<uint8_t>& eti_frame);
};

#endif /* EDI_TO_ETI_H_ */
