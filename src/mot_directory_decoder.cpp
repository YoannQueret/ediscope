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

#include "mot_directory_decoder.h"

#include <cstdio>

#include "tools.h"


// --- MOTDirectoryDecoder -----------------------------------------------------------------
MOTDirectoryDecoder::MOTDirectoryDecoder(MOTDirectoryDecoderObserver *observer) :
		observer(observer),
		dir_transport_id(-1),
		dir_processed_transport_id(-1)
{}

bool MOTDirectoryDecoder::ParseCheckDataGroupHeader(const std::vector<uint8_t>& dg, size_t& offset, int& dg_type) {
	// ETSI EN 300 401 clause 5.3.3.1 - same framing as MOTManager::ParseCheckDataGroupHeader,
	// except accepting type 6 (uncompressed MOT directory) in addition to type 4 (body); type 3
	// (header) isn't used in directory mode, types 5/7 (CA/compressed directory) are out of scope
	if(dg.size() < (offset + 2))
		return false;

	bool extension_flag = dg[offset] & 0x80;
	bool crc_flag = dg[offset] & 0x40;
	bool segment_flag = dg[offset] & 0x20;
	bool user_access_flag = dg[offset] & 0x10;
	dg_type = dg[offset] & 0x0F;
	offset += 2 + (extension_flag ? 2 : 0);

	if(!crc_flag || !segment_flag || !user_access_flag)
		return false;
	if(dg_type != 4 && dg_type != 6)
		return false;

	return true;
}

bool MOTDirectoryDecoder::ParseCheckSessionHeader(const std::vector<uint8_t>& dg, size_t& offset, bool& last_seg, int& seg_number, int& transport_id) {
	// ETSI EN 300 401 clause 5.3.3.2
	if(dg.size() < (offset + 3))
		return false;

	last_seg = dg[offset] & 0x80;
	seg_number = ((dg[offset] & 0x7F) << 8) | dg[offset + 1];
	bool transport_id_flag = dg[offset + 2] & 0x10;
	size_t len_indicator = dg[offset + 2] & 0x0F;
	offset += 3;

	if(!transport_id_flag || len_indicator < 2)
		return false;
	if(dg.size() < (offset + len_indicator))
		return false;

	transport_id = (dg[offset] << 8) | dg[offset + 1];
	offset += len_indicator;

	return true;
}

bool MOTDirectoryDecoder::ParseCheckSegmentationHeader(const std::vector<uint8_t>& dg, size_t& offset, size_t& seg_size) {
	// ETSI EN 301 234 clause 5.1.1 - attached to every segment of every MOT entity, directory
	// included (clause 5.1.3)
	if(dg.size() < (offset + 2))
		return false;

	seg_size = ((dg[offset] & 0x1F) << 8) | dg[offset + 1];
	offset += 2;

	if(seg_size != dg.size() - offset - CalcCRC::CRCLen)
		return false;

	return true;
}

void MOTDirectoryDecoder::MSCDataGroupReceived(const uint8_t *data_ptr, size_t len) {
	std::vector<uint8_t> dg(data_ptr, data_ptr + len);
	size_t offset = 0;

	int dg_type;
	if(!ParseCheckDataGroupHeader(dg, offset, dg_type))
		return;

	bool last_seg;
	int seg_number;
	int transport_id;
	if(!ParseCheckSessionHeader(dg, offset, last_seg, seg_number, transport_id))
		return;

	size_t seg_size;
	if(!ParseCheckSegmentationHeader(dg, offset, seg_size))
		return;

	if(dg_type == 6) {
		if(dir_transport_id != transport_id) {
			dir_transport_id = transport_id;
			dir_entity.Reset();
		}
		dir_entity.AddSeg(seg_number, last_seg, &dg[offset], seg_size);
		if(dir_entity.IsFinished() && dir_processed_transport_id != dir_transport_id) {
			dir_processed_transport_id = dir_transport_id;
			ParseDirectory(dir_entity.GetData());
		}
	} else {	// dg_type == 4
		MOTEntity& body = body_entities[transport_id];
		body.AddSeg(seg_number, last_seg, &dg[offset], seg_size);
		if(body.IsFinished())
			TryCompleteObject(transport_id);
	}
}

void MOTDirectoryDecoder::ParseDirectory(const std::vector<uint8_t>& data) {
	// ETSI EN 301 234 clause 7.2.3, figure 30
	if(data.size() < 13) {
		fprintf(stderr, "MOTDirectoryDecoder: directory too short (%zu bytes) - ignored\n", data.size());
		return;
	}

	size_t offset = 0;
	uint32_t w0 = (uint32_t) data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
	size_t directory_size = w0 & 0x3FFFFFFF;
	offset = 4;

	int num_objects = data[offset] << 8 | data[offset + 1];
	offset += 2;

	offset += 3;	// DataCarouselPeriod - not used
	offset += 2;	// Rfu(1) + Rfa(2) + SegmentSize(13) - not used

	if(offset + 2 > data.size())
		return;
	size_t dir_ext_len = data[offset] << 8 | data[offset + 1];
	offset += 2 + dir_ext_len;	// DirectoryExtension (e.g. SortedHeaderInformation) - not needed

	if(directory_size != data.size())
		fprintf(stderr, "MOTDirectoryDecoder: announced directory size (%zu) doesn't match reassembled size (%zu) - parsing anyway\n", directory_size, data.size());

	std::map<int,MOT_DIR_OBJECT> new_objects;

	for(int i = 0; i < num_objects; i++) {
		if(offset + 9 > data.size()) {
			fprintf(stderr, "MOTDirectoryDecoder: directory entry %d/%d runs past the end - aborting parse\n", i + 1, num_objects);
			break;
		}

		MOT_DIR_OBJECT obj;
		obj.transport_id = data[offset] << 8 | data[offset + 1];

		// header core (ETSI EN 301 234 clause 6.1)
		obj.body_size = (size_t) data[offset + 2] << 20 | data[offset + 3] << 12 | data[offset + 4] << 4 | data[offset + 5] >> 4;
		size_t header_size = (data[offset + 5] & 0x0F) << 9 | data[offset + 6] << 1 | data[offset + 7] >> 7;
		obj.content_type = (data[offset + 7] & 0x7F) >> 1;
		obj.content_sub_type = (data[offset + 7] & 0x01) << 8 | data[offset + 8];

		size_t entry_start = offset;
		size_t entry_end = entry_start + 2 + header_size;	// +2 for the TransportId field itself
		if(entry_end > data.size()) {
			fprintf(stderr, "MOTDirectoryDecoder: directory entry %d/%d header_size exceeds directory buffer - aborting parse\n", i + 1, num_objects);
			break;
		}

		// header extension (same PLI-based parameter coding as MOTManager::ParseCheckHeader)
		for(size_t ext_off = entry_start + 9; ext_off < entry_end;) {
			int pli = data[ext_off] >> 6;
			int param_id = data[ext_off] & 0x3F;
			ext_off++;

			size_t data_len;
			switch(pli) {
			case 0b00:
				data_len = 0;
				break;
			case 0b01:
				data_len = 1;
				break;
			case 0b10:
				data_len = 4;
				break;
			default:
				if(ext_off >= entry_end)
					return;
				{
					bool ext = data[ext_off] & 0x80;
					data_len = data[ext_off] & 0x7F;
					ext_off++;
					if(ext) {
						if(ext_off >= entry_end)
							return;
						data_len = (data_len << 8) + data[ext_off];
						ext_off++;
					}
				}
				break;
			}

			if(ext_off + data_len > entry_end) {
				fprintf(stderr, "MOTDirectoryDecoder: directory entry %d/%d parameter 0x%02X runs past its header - aborting parse\n", i + 1, num_objects, param_id);
				return;
			}

			if(param_id == 0x0C && data_len >= 1) {	// ContentName
				std::string charset_name;
				obj.content_name = CharsetTools::ConvertTextToUTF8(&data[ext_off + 1], data_len - 1, data[ext_off] >> 4, true, &charset_name);
			} else if(param_id == 0x25) {	// ScopeStart (ETSI TS 102 371 clause 6.4.7)
				obj.scope_start.assign(&data[ext_off], &data[ext_off] + data_len);
			} else if(param_id == 0x26) {	// ScopeEnd (ETSI TS 102 371 clause 6.4.8)
				obj.scope_end.assign(&data[ext_off], &data[ext_off] + data_len);
			} else if(param_id == 0x27) {	// ScopeID (ETSI TS 102 371 clause 6.4.9)
				obj.scope_id.assign(&data[ext_off], &data[ext_off] + data_len);
			}

			ext_off += data_len;
		}

		new_objects[obj.transport_id] = obj;
		offset = entry_end;
	}

	dir_objects = new_objects;
	fprintf(stderr, "MOTDirectoryDecoder: directory updated (TransportId %d) - %zu objects\n", dir_transport_id, dir_objects.size());

	// drop per-object state for anything no longer listed, so renamed/removed objects don't
	// linger forever, then try to complete every currently-listed object (its body may already
	// be fully buffered, having arrived before or during this directory's own reassembly)
	for(std::map<int,MOTEntity>::iterator it = body_entities.begin(); it != body_entities.end();) {
		if(new_objects.find(it->first) == new_objects.end())
			it = body_entities.erase(it);
		else
			++it;
	}
	for(std::set<int>::iterator it = completed_transport_ids.begin(); it != completed_transport_ids.end();) {
		if(new_objects.find(*it) == new_objects.end())
			it = completed_transport_ids.erase(it);
		else
			++it;
	}

	for(const std::pair<const int,MOT_DIR_OBJECT>& kv : dir_objects)
		TryCompleteObject(kv.first);
}

void MOTDirectoryDecoder::TryCompleteObject(int transport_id) {
	if(completed_transport_ids.find(transport_id) != completed_transport_ids.end())
		return;

	std::map<int,MOT_DIR_OBJECT>::const_iterator dir_it = dir_objects.find(transport_id);
	if(dir_it == dir_objects.end())
		return;

	std::map<int,MOTEntity>::iterator body_it = body_entities.find(transport_id);
	if(body_it == body_entities.end() || !body_it->second.IsFinished() || body_it->second.GetSize() != dir_it->second.body_size)
		return;

	completed_transport_ids.insert(transport_id);
	std::vector<uint8_t> data = body_it->second.GetData();

	fprintf(stderr, "MOTDirectoryDecoder: object completed - TransportId %d, ContentName '%s', ContentType 0x%02X/0x%03X, %zu bytes\n",
			transport_id, dir_it->second.content_name.c_str(), dir_it->second.content_type, dir_it->second.content_sub_type, data.size());

	if(dir_it->second.content_type == MOT_DIR_OBJECT::CONTENT_TYPE_APPLICATION)
		observer->SPIObjectCompleted(dir_it->second, data);
	else
		observer->SPIOtherObjectCompleted(dir_it->second, data);
}
