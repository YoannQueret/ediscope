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

#ifndef ETI_SOURCE_H_
#define ETI_SOURCE_H_

#include "ensemble_source.h"


// --- ETISource -----------------------------------------------------------------
class ETISource : public EnsembleSource {
private:
	bool CheckFrameCompleted(const SYNC_MAGIC& /*matched_sync_magic*/) {return true;}
public:
	ETISource(std::string filename, EnsembleSourceObserver *observer) : EnsembleSource(filename, observer, "ETI", 6144) {
		AddSyncMagic(1, {0x07, 0x3A, 0xB6}, "FSYNC0");
		AddSyncMagic(1, {0xF8, 0xC5, 0x49}, "FSYNC1");
	}
	~ETISource() {}
};

#endif /* ETI_SOURCE_H_ */
