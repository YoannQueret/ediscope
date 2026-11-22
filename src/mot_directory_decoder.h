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

#ifndef MOT_DIRECTORY_DECODER_H_
#define MOT_DIRECTORY_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "msc_packet_demux.h"
#include "mot_manager.h"


// --- MOT_DIR_OBJECT -----------------------------------------------------------------
// one object as described by a directory entry (ETSI EN 301 234 clause 7.2.3)
struct MOT_DIR_OBJECT {
	int transport_id;
	size_t body_size;
	int content_type;
	int content_sub_type;
	std::string content_name;

	// MOT parameters 0x25/0x26/0x27 (ScopeStart/ScopeEnd/ScopeID, ETSI TS 102 371 clauses
	// 6.4.7-6.4.9), raw bytes, empty if absent - mandatory in practice for PI objects, and an
	// alternative (encoder-dependent) way to identify which ensemble/service a PI or GI object
	// is for/which day it covers, instead of (or in addition to) the equivalent content inside
	// the object itself
	std::vector<uint8_t> scope_start;
	std::vector<uint8_t> scope_end;
	std::vector<uint8_t> scope_id;

	static const int CONTENT_TYPE_APPLICATION = 0x07;	// ETSI TS 101 756 table 17
};


// --- MOTDirectoryDecoderObserver -----------------------------------------------------------------
class MOTDirectoryDecoderObserver {
public:
	virtual ~MOTDirectoryDecoderObserver() {}

	// a ContentType=application (0x07) object completed - for SPI (ETSI TS 102 371), this is a
	// binary-encoded Service/Programme/Group Information object (clause 5)
	virtual void SPIObjectCompleted(const MOT_DIR_OBJECT& /*info*/, const std::vector<uint8_t>& /*data*/) {}
	// any other completed object in the carousel - for SPI, expected to be the service/ensemble
	// logos (ContentType=image, ContentSubType=JFIF or PNG)
	virtual void SPIOtherObjectCompleted(const MOT_DIR_OBJECT& /*info*/, const std::vector<uint8_t>& /*data*/) {}
};


// --- MOTDirectoryDecoder -----------------------------------------------------------------
// Decodes an ETSI EN 301 234 MOT Directory mode carousel (clause 7.2) out of a stream of MSC data
// groups (as reassembled by MSCPacketDemux): the directory itself (data group type 6 only -
// compressed directories, type 7, are out of scope here, as ETSI TS 102 371 clause 6.2 forbids
// them for SPI anyway) and the individual objects' bodies (type 4), matched up by TransportId.
// Object bodies may arrive before, during or after the directory entry that describes them, in
// either order, so both are buffered independently and matched opportunistically.
class MOTDirectoryDecoder : public MSCPacketDemuxObserver {
private:
	MOTDirectoryDecoderObserver *observer;

	int dir_transport_id;
	int dir_processed_transport_id;	// last dir_transport_id actually parsed - a repeat broadcast of
										// an unchanged directory keeps completing the same, already-
										// processed MOTEntity on every single one of its own segments
										// (ETSI EN 301 234 clause 7.2.7.3: same content = same
										// TransportId), so re-running ParseDirectory() every time would
										// otherwise redo all its work (and re-log "directory updated")
										// once per segment of every repeat, not once per actual change
	MOTEntity dir_entity;
	std::map<int,MOT_DIR_OBJECT> dir_objects;	// TransportId -> object info, from the current directory

	std::map<int,MOTEntity> body_entities;	// TransportId -> in-progress/finished body, any TransportId
	std::set<int> completed_transport_ids;	// already matched+emitted - don't re-emit every carousel cycle

	bool ParseCheckDataGroupHeader(const std::vector<uint8_t>& dg, size_t& offset, int& dg_type);
	bool ParseCheckSessionHeader(const std::vector<uint8_t>& dg, size_t& offset, bool& last_seg, int& seg_number, int& transport_id);
	bool ParseCheckSegmentationHeader(const std::vector<uint8_t>& dg, size_t& offset, size_t& seg_size);

	void ParseDirectory(const std::vector<uint8_t>& data);
	void TryCompleteObject(int transport_id);
public:
	MOTDirectoryDecoder(MOTDirectoryDecoderObserver *observer);

	void MSCDataGroupReceived(const uint8_t *data, size_t len) override;
};

#endif /* MOT_DIRECTORY_DECODER_H_ */
