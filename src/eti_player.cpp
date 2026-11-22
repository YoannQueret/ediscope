/*
    DABlin - capital DAB experience
    Copyright (C) 2015-2019 Stefan Pöschel

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

#include "eti_player.h"


// --- ETIPlayer -----------------------------------------------------------------
void ETIPlayer::DecodeFrame(const uint8_t *eti_frame) {
	// FSYNC
	uint32_t fsync = eti_frame[1] << 16 | eti_frame[2] << 8 | eti_frame[3];
	if((fsync != 0x073AB6 && fsync != 0xF8C549) || fsync == prev_fsync) {
		fprintf(stderr, "ETIPlayer: ignored ETI frame with FSYNC = 0x%06X\n", fsync);
		return;
	}
	prev_fsync = fsync;

	// ERR
	if(eti_frame[0] != 0xFF) {
		fprintf(stderr, "ETIPlayer: ignored ETI frame with ERR = 0x%02X\n", eti_frame[0]);
		return;
	}

	if(eti_frame[4] == 0xFF && eti_frame[5] == 0xFF && eti_frame[6] == 0xFF && eti_frame[7] == 0xFF) {
		fprintf(stderr, "ETIPlayer: ignored ETI frame with null transmission\n");
		return;
	}

	bool ficf = eti_frame[5] & 0x80;
	int nst = eti_frame[5] & 0x7F;
	int mid = (eti_frame[6] & 0x18) >> 3;
	int fl = (eti_frame[6] & 0x07) << 8 | eti_frame[7];

	// check header CRC
	size_t header_crc_data_len = 4 + nst * 4 + 2;
	uint16_t header_crc_stored = eti_frame[4 + header_crc_data_len] << 8 | eti_frame[4 + header_crc_data_len + 1];
	uint16_t header_crc_calced = CalcCRC::CalcCRC_CRC16_CCITT.Calc(eti_frame + 4, header_crc_data_len);
	if(header_crc_stored != header_crc_calced) {
		fprintf(stderr, "ETIPlayer: ignored ETI frame due to wrong header CRC\n");
		return;
	}

	int ficl = ficf ? (mid == 3 ? 32 : 24) : 0;

	int subch_bytes = 0;
	int subch_offset = 4 + 4 + nst * 4 + 4;

	// check (MST) CRC
	size_t mst_crc_data_len = (fl - nst - 1) * 4;
	uint16_t mst_crc_stored = eti_frame[subch_offset + mst_crc_data_len] << 8 | eti_frame[subch_offset + mst_crc_data_len + 1];
	uint16_t mst_crc_calced = CalcCRC::CalcCRC_CRC16_CCITT.Calc(eti_frame + subch_offset, mst_crc_data_len);
	if(mst_crc_stored != mst_crc_calced) {
		fprintf(stderr, "ETIPlayer: ignored ETI frame due to wrong (MST) CRC\n");
		return;
	}

	if(ficl) {
		ProcessFIC(eti_frame + subch_offset, ficl * 4);
		subch_offset += ficl * 4;
	}

	// lock only now, as due to the FIC, the audio service may be set
	std::lock_guard<std::mutex> lock(audio_service_mutex);

	// every sub-channel's own offset/length has to be walked regardless of whether an audio
	// service is currently selected, so that independent data_sinks (e.g. SPI) below keep
	// receiving bytes no matter what - or whether anything - is being listened to
	int cur_offset = subch_offset;

	{
		std::lock_guard<std::mutex> data_lock(data_sinks_mutex);

		for(int i = 0; i < nst; i++) {
			int scid = (eti_frame[8 + i*4] & 0xFC) >> 2;
			int stl = (eti_frame[8 + i*4 + 2] & 0x03) << 8 | eti_frame[8 + i*4 + 3];
			int bytes = stl * 8;

			if(!audio_service.IsNone() && scid == audio_service.subchid) {
				subch_offset = cur_offset;
				subch_bytes = bytes;
			}

			std::map<int,SubchannelSink*>::const_iterator sink_it = data_sinks.find(scid);
			if(sink_it != data_sinks.end() && sink_it->second)
				sink_it->second->Feed(eti_frame + cur_offset, bytes);

			cur_offset += bytes;
		}
	}

	if(audio_service.IsNone())
		return;

	if(subch_bytes == 0) {
		fprintf(stderr, "ETIPlayer: ignored ETI frame without sub-channel %d\n", audio_service.subchid);
		return;
	}

	dec->Feed(eti_frame + subch_offset, subch_bytes);
}
