/*
    DABlin - capital DAB experience
    Copyright (C) 2015-2022 Stefan Pöschel

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

#include "fic_decoder.h"


// --- FICDecoder -----------------------------------------------------------------
const std::chrono::seconds FICDecoder::service_timeout{3};

void FICDecoder::Log(const std::string& message) {
	observer->FICLogMessage(message);
}

void FICDecoder::Reset() {
	ensemble = FIC_ENSEMBLE();
	services.clear();
	subchannels.clear();
	data_service_sids.clear();
	data_service_subchids.clear();
	packet_data_service_sids.clear();
	packet_data_service_scids.clear();
	packet_data_subchids.clear();
	packet_data_addresses.clear();
	data_service_ua_types.clear();
	spi_service_sids.clear();
	spi_subchannels.clear();
	packet_mode_fec.clear();
	utc_dt = FIC_DAB_DT();
}

void FICDecoder::Process(const uint8_t *data, size_t len) {
	// check for integer FIB count
	if(len % 32) {
		fprintf(stderr, "FICDecoder: Ignoring non-integer FIB count FIC data with %zu bytes\n", len);
		return;
	}

	for(size_t i = 0; i < len; i += 32)
		ProcessFIB(data + i);
}


void FICDecoder::ProcessFIB(const uint8_t *data) {
	// check CRC
	uint16_t crc_stored = data[30] << 8 | data[31];
	uint16_t crc_calced = CalcCRC::CalcCRC_CRC16_CCITT.Calc(data, 30);
	if(crc_stored != crc_calced) {
		observer->FICDiscardedFIB();
		return;
	}

	// iterate over all FIGs
	for(size_t offset = 0; offset < 30 && data[offset] != 0xFF;) {
		int type = data[offset] >> 5;
		size_t len = data[offset] & 0x1F;
		offset++;

		switch(type) {
		case 0:
			ProcessFIG0(data + offset, len);
			break;
		case 1:
			ProcessFIG1(data + offset, len);
			break;
//		default:
//			fprintf(stderr, "FICDecoder: received unsupported FIG %d with %zu bytes\n", type, len);
		}
		offset += len;
	}
}


void FICDecoder::ProcessFIG0(const uint8_t *data, size_t len) {
	if(len < 1) {
		fprintf(stderr, "FICDecoder: received empty FIG 0\n");
		return;
	}

	// read/skip FIG 0 header
	FIG0_HEADER header(data[0]);
	data++;
	len--;

	// FIG 0/6 (service linking), FIG 0/21 (frequency information) and FIG 0/24 (OE Services) are
	// all part of "service following" (see ETSI TS 103 176) and specifically need what the
	// blanket check below discards for every other extension: 0/6 reuses C/N for database
	// continuation (clause 5.2.2.1 SIV signalling - not current/next mux config) and never sets
	// OE; 0/21 and 0/24 use OE to mean "this ensemble/service" vs. "another ensemble/service, or
	// FM/AM/DRM", which is the whole point of them. All three are handled here, ahead of (and
	// regardless of) the data-mode/cn/oe handling below that the remaining, MCI-flavoured
	// extensions rely on
	if(header.extension == 6) {
		ProcessFIG0_6(data, len, header.pd, !header.cn);
		return;
	}
	if(header.extension == 21) {
		ProcessFIG0_21(data, len, header.oe);
		return;
	}
	if(header.extension == 24) {
		ProcessFIG0_24(data, len, header.oe, header.pd);
		return;
	}

	// ignore next config/other ensembles
	if(header.cn || header.oe)
		return;

	if(header.pd) {
		// data mode (P/D=1): FIG 0/2 resolves the SId of a plain data service (e.g. a
		// locally-configured EPG sub-channel), FIG 0/13 signals which user application (if
		// any) runs on it - everything else keeps being ignored, as its data-mode variant
		// (if any) isn't verified/implemented
		switch(header.extension) {
		case 2:
			ProcessFIG0_2_Data(data, len);
			break;
		case 13:
			ProcessFIG0_13_Data(data, len);
			break;
		}
		return;
	}

	// handle extension
	switch(header.extension) {
	case 0:
		ProcessFIG0_0(data, len);
		break;
	case 1:
		ProcessFIG0_1(data, len);
		break;
	case 2:
		ProcessFIG0_2(data, len);
		break;
	case 3:
		ProcessFIG0_3(data, len);
		break;
	case 5:
		ProcessFIG0_5(data, len);
		break;
	case 8:
		ProcessFIG0_8(data, len);
		break;
	case 9:
		ProcessFIG0_9(data, len);
		break;
	case 10:
		ProcessFIG0_10(data, len);
		break;
	case 13:
		ProcessFIG0_13(data, len);
		break;
	case 14:
		ProcessFIG0_14(data, len);
		break;
	case 17:
		ProcessFIG0_17(data, len);
		break;
	case 18:
		ProcessFIG0_18(data, len);
		break;
	case 19:
		ProcessFIG0_19(data, len);
		break;
//	default:
//		fprintf(stderr, "FICDecoder: received unsupported FIG 0/%d with %zu field bytes\n", header.extension, len);
	}
}

void FICDecoder::ProcessFIG0_0(const uint8_t *data, size_t len) {
	// FIG 0/0 - Ensemble information
	// EId and alarm flag only

	if(len < 4)
		return;

	FIC_ENSEMBLE new_ensemble = ensemble;
	new_ensemble.eid = data[0] << 8 | data[1];
	new_ensemble.al_flag = data[2] & 0x20;

	if(ensemble != new_ensemble) {
		ensemble = new_ensemble;

		fprintf(stderr, "FICDecoder: EId 0x%04X: alarm flag: %s\n",
				ensemble.eid, ensemble.al_flag ? "true" : "false");
		Log("EId " + StringTools::IntToHex(ensemble.eid, 4) + ": alarm flag: " + (ensemble.al_flag ? "true" : "false"));

		UpdateEnsemble();
	}
}

void FICDecoder::ProcessFIG0_1(const uint8_t *data, size_t len) {
	// FIG 0/1 - Basic sub-channel organization

	// iterate through all sub-channels
	for(size_t offset = 0; offset < len;) {
		int subchid = data[offset] >> 2;
		size_t start_address = (data[offset] & 0x03) << 8 | data[offset + 1];
		offset += 2;

		FIC_SUBCHANNEL sc;
		sc.start = start_address;

		bool short_long_form = data[offset] & 0x80;
		if(short_long_form) {
			// long form
			int option = (data[offset] & 0x70) >> 4;
			int pl = (data[offset] & 0x0C) >> 2;
			size_t subch_size = (data[offset] & 0x03) << 8 | data[offset + 1];

			switch(option) {
			case 0b000:
				sc.size = subch_size;
				sc.pl = "EEP " + std::to_string(pl + 1) + "-A";
				sc.bitrate = subch_size / eep_a_size_factors[pl] *  8;
				break;
			case 0b001:
				sc.size = subch_size;
				sc.pl = "EEP " + std::to_string(pl + 1) + "-B";
				sc.bitrate = subch_size / eep_b_size_factors[pl] * 32;
				break;
			}
			offset += 2;
		} else {
			// short form

			bool table_switch = data[offset] & 0x40;
			if(!table_switch) {
				int table_index = data[offset] & 0x3F;
				sc.size = uep_sizes[table_index];
				sc.pl = "UEP " + std::to_string(uep_pls[table_index]);
				sc.bitrate = uep_bitrates[table_index];
			}
			offset++;
		}

		if(!sc.IsNone()) {
			FIC_SUBCHANNEL& current_sc = GetSubchannel(subchid);
			sc.language = current_sc.language;	// ignored for comparison
			bool changed = current_sc != sc;
			if(changed) {
				current_sc = sc;

				fprintf(stderr, "FICDecoder: SubChId %2d: start %3zu CUs, size %3zu CUs, PL %-7s = %3d kBit/s\n", subchid, sc.start, sc.size, sc.pl.c_str(), sc.bitrate);
				Log("SubChId " + std::to_string(subchid) + ": start " + std::to_string(sc.start) + " CUs, size " + std::to_string(sc.size) +
						" CUs, PL " + sc.pl + " = " + std::to_string(sc.bitrate) + " kBit/s");
			}

			// FIG 0/1 lists the complete, currently valid sub-channel table on every cycle
			// (ETSI EN 300 401 clause 6.3), so its (re)appearance is the liveness signal
			// used to detect a sub-channel being hot-removed - same pattern as FIG 0/2 for services
			current_sc.seen_in_fig0_1 = true;
			current_sc.last_seen_fig0_1 = std::chrono::steady_clock::now();

			if(changed)
				UpdateSubchannel(subchid);
		}
	}
}

void FICDecoder::ProcessFIG0_2(const uint8_t *data, size_t len) {
	// FIG 0/2 - Basic service and service component definition
	// programme services only

	// iterate through all services
	for(size_t offset = 0; offset < len;) {
		uint16_t sid = data[offset] << 8 | data[offset + 1];
		offset += 2;

		size_t num_service_comps = data[offset++] & 0x0F;

		// FIG 0/2 lists all currently valid services on every cycle, so its (re)appearance
		// is used as the liveness signal to detect a service being hot-removed from the ensemble
		FIC_SERVICE& live_service = GetService(sid);
		live_service.seen_in_fig0_2 = true;
		live_service.last_seen_fig0_2 = std::chrono::steady_clock::now();

		// iterate through all service components
		for(size_t comp = 0; comp < num_service_comps; comp++) {
			int tmid = data[offset] >> 6;

			switch(tmid) {
			case 0b00:	// MSC stream audio
				int ascty = data[offset] & 0x3F;
				int subchid = data[offset + 1] >> 2;
				bool ps = data[offset + 1] & 0x02;
				bool ca = data[offset + 1] & 0x01;

				if(!ca) {
					switch(ascty) {
					case 0:		// DAB
					case 63:	// DAB+
						bool dab_plus = ascty == 63;

						AUDIO_SERVICE audio_service(subchid, dab_plus);

						FIC_SERVICE& service = GetService(sid);
						AUDIO_SERVICE& current_audio_service = service.audio_comps[subchid];
						if(current_audio_service != audio_service || ps != (service.pri_comp_subchid == subchid)) {
							current_audio_service = audio_service;
							if(ps)
								service.pri_comp_subchid = subchid;

							fprintf(stderr, "FICDecoder: SId 0x%04X: audio service (SubChId %2d, %-4s, %s)\n", sid, subchid, dab_plus ? "DAB+" : "DAB", ps ? "primary" : "secondary");
							Log("SId " + StringTools::IntToHex(sid, 4) + ": audio service (SubChId " + std::to_string(subchid) + ", " +
									(dab_plus ? "DAB+" : "DAB") + ", " + (ps ? "primary" : "secondary") + ")");

							UpdateService(service);
						}

						break;
					}
				}
			}

			offset += 2;
		}
	}
}

void FICDecoder::SetDataServiceSid(int subchid, uint32_t sid) {
	uint32_t& current_sid = data_service_sids[subchid];
	if(current_sid == sid)
		return;
	current_sid = sid;
	data_service_subchids[sid] = subchid;

	fprintf(stderr, "FICDecoder: SubChId %2d: data service SId 0x%08X\n", subchid, sid);
	Log("SubChId " + std::to_string(subchid) + ": data service SId " + StringTools::IntToHex((int) sid, sid > 0xFFFF ? 8 : 4));

	observer->FICChangeDataServiceSID(subchid, sid);

	// the SubChId possibly changed under an already-known SPI SId (e.g. a mux-side hot
	// reconfiguration) - re-resolve so FICChangeSPIService fires again with the right one
	ResolveSPIService(sid);
}

void FICDecoder::SetDataServiceUAType(uint32_t sid, int ua_type) {
	std::map<uint32_t,int>::iterator it = data_service_ua_types.find(sid);
	if(it != data_service_ua_types.end() && it->second == ua_type)
		return;
	data_service_ua_types[sid] = ua_type;

	fprintf(stderr, "FICDecoder: SId 0x%s: data service user application %s\n",
			StringTools::SidToHex((int) sid).c_str(), ConvertUATypeToString(ua_type).c_str());
	Log("SId " + StringTools::SidToHex((int) sid) + ": data service user application " + ConvertUATypeToString(ua_type));

	observer->FICChangeDataServiceUAType(sid, ua_type);
}

void FICDecoder::ResolvePacketDataService(int scid) {
	// only once both halves are known: the SId (from FIG 0/2, data mode) and the SubChId
	// (from FIG 0/3) that this SCId's packet component actually maps to
	std::map<int,uint32_t>::const_iterator sid_it = packet_data_service_sids.find(scid);
	std::map<int,int>::const_iterator subchid_it = packet_data_subchids.find(scid);
	if(sid_it != packet_data_service_sids.end() && subchid_it != packet_data_subchids.end())
		SetDataServiceSid(subchid_it->second, sid_it->second);

	if(sid_it != packet_data_service_sids.end())
		ResolveSPIService(sid_it->second);
}

void FICDecoder::ResolveSPIService(uint32_t sid) {
	// only handles the primary component (SCIdS 0) of a data service, i.e. the common case
	// where the SPI/EPG user application is the sole component of its own data service (see
	// the same simplification already made for FIG 0/2 data mode, above)
	if(spi_service_sids.find(sid) == spi_service_sids.end())
		return;

	int subchid = -1;
	std::map<uint32_t,int>::const_iterator subchid_it = data_service_subchids.find(sid);
	if(subchid_it != data_service_subchids.end())
		subchid = subchid_it->second;

	int address = -1;
	std::map<uint32_t,int>::const_iterator scid_it = packet_data_service_scids.find(sid);
	if(scid_it != packet_data_service_scids.end()) {
		std::map<int,int>::const_iterator address_it = packet_data_addresses.find(scid_it->second);
		if(address_it != packet_data_addresses.end())
			address = address_it->second;
	}

	if(subchid == -1 || address == -1)
		return;

	int& current_address = spi_subchannels[subchid];
	if(current_address == address)
		return;
	current_address = address;

	bool fec = packet_mode_fec.count(subchid) ? packet_mode_fec[subchid] : false;

	fprintf(stderr, "FICDecoder: SubChId %2d: SPI service (packet mode, address %d, FEC %s)\n", subchid, address, fec ? "applied" : "not applied");
	Log("SubChId " + std::to_string(subchid) + ": SPI service (packet mode, address " + std::to_string(address) + ", FEC " + (fec ? "applied" : "not applied") + ")");

	observer->FICChangeSPIService(subchid, address, fec);
}

void FICDecoder::RemoveSPIService(int subchid) {
	if(spi_subchannels.erase(subchid)) {
		fprintf(stderr, "FICDecoder: SubChId %2d: SPI service removed\n", subchid);
		Log("SubChId " + std::to_string(subchid) + ": SPI service removed");

		observer->FICRemoveSPIService(subchid);
	}
}

void FICDecoder::ProcessFIG0_2_Data(const uint8_t *data, size_t len) {
	// FIG 0/2 (data mode, P/D=1) - plain data service (e.g. EPG) + component definition.
	// Mirrors ProcessFIG0_2's structure, but with the data-mode service header (32-bit SId).
	// "MSC stream data" (TMid=1) primary components give the SubChId directly; "MSC packet
	// data" (TMid=3) ones only give an SCId, resolved to a SubChId via FIG 0/3 instead.

	// iterate through all data services
	for(size_t offset = 0; offset < len;) {
		uint32_t sid32 = (uint32_t) data[offset] << 24 | (uint32_t) data[offset + 1] << 16 |
				(uint32_t) data[offset + 2] << 8 | data[offset + 3];
		offset += 4;

		size_t num_service_comps = data[offset++] & 0x0F;

		// iterate through all service components
		for(size_t comp = 0; comp < num_service_comps; comp++) {
			int tmid = data[offset] >> 6;
			bool ps = data[offset + 1] & 0x02;

			if(ps) {	// only the primary component identifies the service's own sub-channel
				if(tmid == 0b01) {	// MSC stream data
					int subchid = data[offset + 1] >> 2;
					SetDataServiceSid(subchid, sid32);
				} else if(tmid == 0b11) {	// MSC packet data
					int scid = (data[offset] & 0x3F) << 6 | (data[offset + 1] >> 2);
					packet_data_service_sids[scid] = sid32;
					packet_data_service_scids[sid32] = scid;
					ResolvePacketDataService(scid);
				}
			}

			offset += 2;
		}
	}
}

void FICDecoder::ProcessFIG0_3(const uint8_t *data, size_t len) {
	// FIG 0/3 - Service component definition in packet mode: SCId -> SubChId + packet address.
	// Needed to resolve the sub-channel (and, for SPI, the packet address) of a "MSC packet
	// data" (TMid=3) data service component found in FIG 0/2 (data mode).

	for(size_t offset = 0; offset + 5 <= len;) {
		int scid = data[offset] << 4 | (data[offset + 1] >> 4);
		bool scca_flag = data[offset + 1] & 0x01;
		int subchid = data[offset + 3] >> 2;
		int address = (data[offset + 3] & 0x03) << 8 | data[offset + 4];
		offset += 5;
		if(scca_flag)
			offset += 2;	// CAOrg field, not used

		bool changed = false;
		if(packet_data_subchids[scid] != subchid) {
			packet_data_subchids[scid] = subchid;
			changed = true;
		}
		if(packet_data_addresses[scid] != address) {
			packet_data_addresses[scid] = address;
			changed = true;
		}
		if(changed)
			ResolvePacketDataService(scid);
	}
}

void FICDecoder::ProcessFIG0_5(const uint8_t *data, size_t len) {
	// FIG 0/5 - Service component language
	// programme services only

	// iterate through all components
	for(size_t offset = 0; offset < len;) {
		bool ls_flag = data[offset] & 0x80;
		if(ls_flag) {
			// long form - skipped, as not relevant
			offset += 3;
		} else {
			// short form
			bool msc_fic_flag = data[offset] & 0x40;

			// handle only MSC components
			if(!msc_fic_flag) {
				int subchid = data[offset] & 0x3F;
				int language = data[offset + 1];

				FIC_SUBCHANNEL& current_sc = GetSubchannel(subchid);
				if(current_sc.language != language) {
					current_sc.language = language;

					fprintf(stderr, "FICDecoder: SubChId %2d: language '%s'\n", subchid, ConvertLanguageToString(language).c_str());
					Log("SubChId " + std::to_string(subchid) + ": language '" + ConvertLanguageToString(language) + "'");

					UpdateSubchannel(subchid);
				}
			}

			offset += 2;
		}
	}
}

void FICDecoder::ProcessFIG0_8(const uint8_t *data, size_t len) {
	// FIG 0/8 - Service component global definition
	// programme services only

	// iterate through all service components
	for(size_t offset = 0; offset < len;) {
		uint16_t sid = data[offset] << 8 | data[offset + 1];
		offset += 2;

		bool ext_flag = data[offset] & 0x80;
		int scids = data[offset] & 0x0F;
		offset++;

		bool ls_flag = data[offset] & 0x80;
		if(ls_flag) {
			// long form - skipped, as not relevant
			offset += 2;
		} else {
			// short form
			bool msc_fic_flag = data[offset] & 0x40;

			// handle only MSC components
			if(!msc_fic_flag) {
				int subchid = data[offset] & 0x3F;

				FIC_SERVICE& service = GetService(sid);
				bool new_comp = service.comp_defs.find(scids) == service.comp_defs.end();
				int& current_subchid = service.comp_defs[scids];
				if(new_comp || current_subchid != subchid) {
					current_subchid = subchid;

					fprintf(stderr, "FICDecoder: SId 0x%04X, SCIdS %2d: MSC service component (SubChId %2d)\n", sid, scids, subchid);
					Log("SId " + StringTools::IntToHex(sid, 4) + ", SCIdS " + std::to_string(scids) + ": MSC service component (SubChId " + std::to_string(subchid) + ")");

					UpdateService(service);
				}
			}

			offset++;
		}

		// skip Rfa field, if needed
		if(ext_flag)
			offset++;
	}
}

void FICDecoder::ProcessFIG0_9(const uint8_t *data, size_t len) {
	// FIG 0/9 - Time and country identifier - Country, LTO and International table
	// ensemble ECC/LTO/international table ID, plus optional extended fields listing
	// services whose ECC differs from the ensemble's (bit 7 of the first byte: Ext)

	if(len < 3)
		return;

	bool ext = data[0] & 0x80;

	FIC_ENSEMBLE new_ensemble = ensemble;
	new_ensemble.lto = (data[0] & 0x20 ? -1 : 1) * (data[0] & 0x1F);
	new_ensemble.ecc = data[1];
	new_ensemble.inter_table_id = data[2];

	bool ensemble_changed = ensemble != new_ensemble;
	if(ensemble_changed) {
		ensemble = new_ensemble;

		fprintf(stderr, "FICDecoder: ECC: 0x%02X, LTO: %s, international table ID: 0x%02X (%s)\n",
				ensemble.ecc, ConvertLTOToString(ensemble.lto).c_str(), ensemble.inter_table_id, ConvertInterTableIDToString(ensemble.inter_table_id).c_str());
		Log("ECC: " + StringTools::IntToHex(ensemble.ecc, 2) + ", LTO: " + ConvertLTOToString(ensemble.lto) +
				", international table ID: " + StringTools::IntToHex(ensemble.inter_table_id, 2) + " (" + ConvertInterTableIDToString(ensemble.inter_table_id) + ")");

		UpdateEnsemble();

		// update services that changes to the ensemble's own ECC/table may affect
		for(const fic_services_t::value_type& service : services) {
			const FIC_SERVICE& s = service.second;
			if(s.pty_static != FIC_SERVICE::pty_none || s.pty_dynamic != FIC_SERVICE::pty_none)
				UpdateService(s);
		}
	}

	// parse per-service ECC exceptions, if any; each subfield is: 1 byte (top 2 bits: number
	// of SIds, 0-3) + 1 byte ECC + that many 16-bit SIds - repeated until the FIG is exhausted
	std::set<uint16_t> excepted_sids;
	if(ext) {
		for(size_t offset = 3; offset + 2 <= len;) {
			int num_sids = data[offset] >> 6;
			int subfield_ecc = data[offset + 1];
			offset += 2;

			for(int i = 0; i < num_sids && offset + 2 <= len; i++) {
				uint16_t sid = data[offset] << 8 | data[offset + 1];
				offset += 2;

				excepted_sids.insert(sid);

				FIC_SERVICE& service = GetService(sid);
				if(service.ecc != subfield_ecc) {
					service.ecc = subfield_ecc;

					fprintf(stderr, "FICDecoder: SId 0x%04X: ECC 0x%02X\n", sid, subfield_ecc);
					Log("SId " + StringTools::IntToHex(sid, 4) + ": ECC " + StringTools::IntToHex(subfield_ecc, 2).substr(2));

					UpdateService(service);
				}
			}
		}
	}

	// any service no longer listed as an exception is back to the ensemble's own ECC
	for(fic_services_t::value_type& kv : services) {
		FIC_SERVICE& service = kv.second;
		if(service.ecc != FIC_SERVICE::ecc_none && excepted_sids.find(service.sid) == excepted_sids.end()) {
			service.ecc = FIC_SERVICE::ecc_none;
			UpdateService(service);
		}
	}
}

void FICDecoder::ProcessFIG0_10(const uint8_t *data, size_t len) {
	// FIG 0/10 - Date and time (d&t)

	if(len < 4)
		return;

	FIC_DAB_DT new_utc_dt;

	// ignore short form, once long form available
	bool utc_flag = data[2] & 0x08;
	if(!utc_flag && utc_dt_long)
		return;

	// retrieve date
	int mjd = (data[0] & 0x7F) << 10 | data[1] << 2 | data[2] >> 6;

	int y0 = floor((mjd - 15078.2) / 365.25);
	int m0 = floor((mjd - 14956.1 - floor(y0 * 365.25)) / 30.6001);
	int d = mjd - 14956 - floor(y0 * 365.25) - floor(m0 * 30.6001);
	int k = (m0 == 14 || m0 == 15) ? 1 : 0;
	int y = y0 + k;
	int m = m0 - 1 - k * 12;

	new_utc_dt.dt.tm_year = y;		// from 1900
	new_utc_dt.dt.tm_mon = m - 1;	// 0-based
	new_utc_dt.dt.tm_mday = d;

	// retrieve time
	new_utc_dt.dt.tm_hour = (data[2] & 0x07) << 2 | data[3] >> 6;
	new_utc_dt.dt.tm_min = data[3] & 0x3F;
	new_utc_dt.dt.tm_isdst = -1;	// ignore DST
	if(utc_flag) {
		// long form
		if(len < 6)
			return;
		new_utc_dt.dt.tm_sec = data[4] >> 2;
		new_utc_dt.ms = (data[4] & 0x03) << 8 | data[5];
		utc_dt_long = true;
	} else {
		// short form
		new_utc_dt.dt.tm_sec = 0;
		new_utc_dt.ms = FIC_DAB_DT::ms_none;
	}

	if(utc_dt != new_utc_dt) {
		// print only once (or once again on precision change)
		if(utc_dt.IsNone() || utc_dt.IsMsNone() != new_utc_dt.IsMsNone())
			fprintf(stderr, "FICDecoder: UTC date/time: %s\n", ConvertDateTimeToString(new_utc_dt, 0, true).c_str());

		utc_dt = new_utc_dt;

		observer->FICChangeUTCDateTime(utc_dt);
	}
}

void FICDecoder::ProcessFIG0_13(const uint8_t *data, size_t len) {
	// FIG 0/13 - User application information
	// programme services only

	// iterate through all service components
	for(size_t offset = 0; offset < len;) {
		uint16_t sid = data[offset] << 8 | data[offset + 1];
		offset += 2;

		int scids = data[offset] >> 4;
		size_t num_scids_uas = data[offset] & 0x0F;
		offset++;

		// iterate through all user applications
		for(size_t scids_ua = 0; scids_ua < num_scids_uas; scids_ua++) {
			int ua_type = data[offset] << 3 | data[offset + 1] >> 5;
			size_t ua_data_length = data[offset + 1] & 0x1F;
			offset += 2;

			// handle only Slideshow
			if(ua_type == 0x002) {
				FIC_SERVICE& service = GetService(sid);
				if(service.comp_sls_uas.find(scids) == service.comp_sls_uas.end()) {
					ua_data_t& sls_ua_data = service.comp_sls_uas[scids];

					sls_ua_data.resize(ua_data_length);
					if(ua_data_length)
						memcpy(&sls_ua_data[0], data + offset, ua_data_length);

					fprintf(stderr, "FICDecoder: SId 0x%04X, SCIdS %2d: Slideshow (%zu bytes UA data)\n", sid, scids, ua_data_length);
					Log("SId " + StringTools::IntToHex(sid, 4) + ", SCIdS " + std::to_string(scids) + ": Slideshow (" + std::to_string(ua_data_length) + " bytes UA data)");

					UpdateService(service);
				}
			}

			offset += ua_data_length;
		}
	}
}

void FICDecoder::ProcessFIG0_13_Data(const uint8_t *data, size_t len) {
	// FIG 0/13 (data mode, P/D=1) - User application information for a data service (e.g. SPI)
	// only handles the primary component (SCIdS 0), as with FIG 0/2 data mode above; there's
	// no X-PAD data field here (that only applies to applications carried in an audio
	// service's X-PAD, never to a data service's own component)

	static const int ua_type_epg = 0x007;	// ETSI TS 101 756 table 16, ETSI TS 102 818 (SPI)

	for(size_t offset = 0; offset + 5 <= len;) {
		uint32_t sid = (uint32_t) data[offset] << 24 | (uint32_t) data[offset + 1] << 16 |
				(uint32_t) data[offset + 2] << 8 | data[offset + 3];
		offset += 4;

		int scids = data[offset] >> 4;
		size_t num_scids_uas = data[offset] & 0x0F;
		offset++;

		for(size_t scids_ua = 0; scids_ua < num_scids_uas && offset + 2 <= len; scids_ua++) {
			int ua_type = data[offset] << 3 | data[offset + 1] >> 5;
			size_t ua_data_length = data[offset + 1] & 0x1F;
			offset += 2;

			if(scids == 0) {
				SetDataServiceUAType(sid, ua_type);

				if(ua_type == ua_type_epg && spi_service_sids.insert(sid).second)
					ResolveSPIService(sid);
			}

			offset += ua_data_length;
		}
	}
}

void FICDecoder::ProcessFIG0_14(const uint8_t *data, size_t len) {
	// FIG 0/14 - FEC sub-channel organization: SubChId -> FEC scheme applied (0/1; 2/3 reserved)

	for(size_t offset = 0; offset < len; offset++) {
		int subchid = data[offset] >> 2;
		bool fec = (data[offset] & 0x03) == 0x01;

		bool& current_fec = packet_mode_fec[subchid];
		if(current_fec != fec) {
			current_fec = fec;

			fprintf(stderr, "FICDecoder: SubChId %2d: FEC scheme %s\n", subchid, fec ? "applied" : "not applied");
			Log("SubChId " + std::to_string(subchid) + ": FEC scheme " + (fec ? "applied" : "not applied"));

			// the FEC status of a sub-channel already confirmed as SPI may have just changed
			std::map<int,int>::const_iterator spi_it = spi_subchannels.find(subchid);
			if(spi_it != spi_subchannels.end())
				observer->FICChangeSPIService(subchid, spi_it->second, fec);
		}
	}
}

void FICDecoder::ProcessFIG0_17(const uint8_t *data, size_t len) {
	// FIG 0/17 - Programme Type
	// programme type only

	// iterate through all services
	for(size_t offset = 0; offset < len;) {
		uint16_t sid = data[offset] << 8 | data[offset + 1];
		bool sd = data[offset + 2] & 0x80;
		bool l_flag = data[offset + 2] & 0x20;
		bool cc_flag = data[offset + 2] & 0x10;
		offset += 3;

		// skip language, if present
		if(l_flag)
			offset++;

		// programme type (international code)
		int pty = data[offset] & 0x1F;
		offset++;

		// skip CC part, if present
		if(cc_flag)
			offset++;

		FIC_SERVICE& service = GetService(sid);
		int& current_pty = sd ? service.pty_dynamic : service.pty_static;
		if(current_pty != pty) {
			// suppress message, if dynamic FIC messages disabled and dynamic PTY not initally be set
			bool show_msg = !(disable_dyn_msgs && sd && current_pty != FIC_SERVICE::pty_none);

			current_pty = pty;

			if(show_msg) {
				// assuming international table ID 0x01 here!
				fprintf(stderr, "FICDecoder: SId 0x%04X: programme type (%s): '%s'\n",
						sid, sd ? "dynamic" : "static", ConvertPTYToString(pty, 0x01).c_str());
				Log("SId " + StringTools::IntToHex(sid, 4) + ": programme type (" + (sd ? "dynamic" : "static") + "): '" + ConvertPTYToString(pty, 0x01) + "'");
			}

			UpdateService(service);
		}
	}
}

void FICDecoder::ProcessFIG0_18(const uint8_t *data, size_t len) {
	// FIG 0/18 - Announcement support

	// iterate through all services
	for(size_t offset = 0; offset < len;) {
		uint16_t sid = data[offset] << 8 | data[offset + 1];
		uint16_t asu_flags = data[offset + 2] << 8 | data[offset + 3];
		size_t number_of_clusters = data[offset + 4] & 0x1F;
		offset += 5;

		cids_t cids;
		for(size_t i = 0; i < number_of_clusters; i++)
			cids.emplace(data[offset++]);

		FIC_SERVICE& service = GetService(sid);
		uint16_t& current_asu_flags = service.asu_flags;
		cids_t& current_cids = service.cids;
		if(current_asu_flags != asu_flags || current_cids != cids) {
			current_asu_flags = asu_flags;
			current_cids = cids;

			std::string cids_str;
			char cid_string[5];
			for(const cids_t::value_type& cid : cids) {
				if(!cids_str.empty())
					cids_str += "/";
				snprintf(cid_string, sizeof(cid_string), "0x%02X", cid);
				cids_str += std::string(cid_string);
			}

			fprintf(stderr, "FICDecoder: SId 0x%04X: ASu flags 0x%04X, cluster(s) %s\n",
					sid, asu_flags, cids_str.c_str());
			Log("SId " + StringTools::IntToHex(sid, 4) + ": ASu flags " + StringTools::IntToHex(asu_flags, 4) + ", cluster(s) " + cids_str);

			UpdateService(service);
		}
	}
}

void FICDecoder::ProcessFIG0_19(const uint8_t *data, size_t len) {
	// FIG 0/19 - Announcement switching

	// iterate through all announcement clusters
	for(size_t offset = 0; offset < len;) {
		uint8_t cid = data[offset];
		uint16_t asw_flags = data[offset + 1] << 8 | data[offset + 2];
		bool region_flag = data[offset + 3] & 0x40;
		int subchid = data[offset + 3] & 0x3F;
		offset += region_flag ? 5 : 4;

		FIC_ASW_CLUSTER ac;
		ac.asw_flags = asw_flags;
		ac.subchid = subchid;

		FIC_ASW_CLUSTER& current_ac = ensemble.asw_clusters[cid];
		if(current_ac != ac) {
			current_ac = ac;

			if(!disable_dyn_msgs) {
				fprintf(stderr, "FICDecoder: ASw cluster 0x%02X: flags 0x%04X, SubChId %2d\n",
						cid, asw_flags, subchid);
				Log("ASw cluster " + StringTools::IntToHex(cid, 2) + ": flags " + StringTools::IntToHex(asw_flags, 4) + ", SubChId " + std::to_string(subchid));
			}

			UpdateEnsemble();

			// update services that changes may affect
			for(const fic_services_t::value_type& service : services) {
				const FIC_SERVICE& s = service.second;
				if(s.cids.find(cid) != s.cids.cend())
					UpdateService(s);
			}
		}
	}
}

int FICDecoder::ServiceLinkingKey(bool pd, bool hard_link, bool international, int lsn) {
	// ETSI EN 300 401 clause 8.1.15: "The database key comprises the OE and P/D flags (...) and
	// the S/H, ILS, and LSN fields" - OE is always 0 for this extension (reserved), so left out
	return (pd ? 0x8000 : 0) | (hard_link ? 0x4000 : 0) | (international ? 0x2000 : 0) | lsn;
}

void FICDecoder::ProcessFIG0_6(const uint8_t *data, size_t len, bool pd, bool first_in_db) {
	// FIG 0/6 - Service linking information (ETSI EN 300 401 clause 8.1.15)

	for(size_t offset = 0; offset + 2 <= len;) {
		bool id_list_flag = data[offset] & 0x80;
		bool la = data[offset] & 0x40;
		bool sh = data[offset] & 0x20;
		bool ils = data[offset] & 0x10;
		int lsn = (data[offset] & 0x0F) << 8 | data[offset + 1];
		offset += 2;

		if(lsn == FIC_SERVICE_LINKING_SET::lsn_none)
			continue;	// reserved LSN value - malformed, but offset is already past this entry's fixed header

		int key = ServiceLinkingKey(pd, sh, ils, lsn);

		if(!id_list_flag) {
			// CEI ("Id List flag = 0"): this set's database changed - drop whatever was cached
			// for it so it gets rebuilt cleanly from the next full (non-CEI) occurrence, instead
			// of silently going stale
			service_linking_sets.erase(key);
			continue;
		}

		if(offset + 1 > len)
			break;	// malformed - not enough room for the promised extra byte
		int idlq = (data[offset] >> 5) & 0x03;
		int number_of_ids = data[offset] & 0x0F;
		offset++;

		FIC_SERVICE_LINKING_SET new_set = service_linking_sets[key];	// starts default-constructed if new
		new_set.lsn = lsn;
		new_set.active = la;
		new_set.hard_link = sh;
		new_set.international = ils;
		new_set.is_data = pd;
		if(first_in_db)
			new_set.members.clear();	// fresh start for this key - rebuild from here on

		for(int i = 0; i < number_of_ids; i++) {
			bool is_key_entry = first_in_db && i == 0;

			if(!pd) {
				int ecc = FIC_LINKED_SERVICE::ecc_none;
				if(ils) {
					if(offset + 1 > len)
						break;
					ecc = data[offset];
					offset++;
				}
				if(offset + 2 > len)
					break;
				uint16_t id16 = data[offset] << 8 | data[offset + 1];
				offset += 2;

				if(is_key_entry) {
					new_set.key_service_sid = id16;
				} else {
					FIC_LINKED_SERVICE entry;
					entry.id = id16;
					entry.ecc = ecc;
					switch(idlq) {
					case 0: entry.type = FIC_LINKED_SERVICE::DAB_SID; break;
					case 1: entry.type = FIC_LINKED_SERVICE::RDS_PI; break;
					default: entry.type = FIC_LINKED_SERVICE::DRM_AMSS_ID; break;	// "11" (and reserved "10")
					}
					new_set.members.push_back(entry);
				}
			} else {
				if(offset + 4 > len)
					break;
				uint32_t id32 = (uint32_t) data[offset] << 24 | data[offset + 1] << 16 | data[offset + 2] << 8 | data[offset + 3];
				offset += 4;

				if(is_key_entry) {
					new_set.key_service_sid = id32;
				} else {
					FIC_LINKED_SERVICE entry;
					entry.type = FIC_LINKED_SERVICE::DATA_SID;
					entry.id = id32;
					new_set.members.push_back(entry);
				}
			}
		}

		FIC_SERVICE_LINKING_SET& current_set = service_linking_sets[key];
		if(current_set != new_set) {
			current_set = new_set;

			fprintf(stderr, "FICDecoder: linkage set LSN 0x%03X (%s, %s%s): key service SId %s, %zu other member(s)\n",
					current_set.lsn, current_set.active ? "active" : "inactive", current_set.hard_link ? "hard link" : "soft link",
					current_set.international ? ", international" : "",
					current_set.key_service_sid != FIC_SERVICE_LINKING_SET::sid_none ? StringTools::IntToHex(current_set.key_service_sid, 4).c_str() : "unknown",
					current_set.members.size());
			Log("Linkage set LSN " + StringTools::IntToHex(current_set.lsn, 3) + " (" + (current_set.active ? "active" : "inactive") + ", " +
					(current_set.hard_link ? "hard link" : "soft link") + (current_set.international ? ", international" : "") + "): " +
					std::to_string(current_set.members.size()) + " other member(s)");

			observer->FICChangeServiceLinking(key, current_set);
		}
	}
}

int FICDecoder::FrequencyInfoKey(bool oe, uint32_t id, int rm) {
	// ETSI EN 300 401 clause 8.1.8: "The database key comprises the OE and P/D flags (...) and
	// the Rfa, Id field, and R&M fields" - P/D and Rfa are always 0 for this extension (reserved)
	return (oe ? (1 << 20) : 0) | (int) (id << 4) | rm;
}

void FICDecoder::ProcessFIG0_21(const uint8_t *data, size_t len, bool oe) {
	// FIG 0/21 - Frequency Information (ETSI EN 300 401 clause 8.1.8)

	for(size_t group_offset = 0; group_offset + 2 <= len;) {
		size_t length_of_fi_list = (data[group_offset] << 8 | data[group_offset + 1]) & 0x1F;
		group_offset += 2;

		size_t fi_list_end = group_offset + length_of_fi_list;
		if(fi_list_end > len)
			break;	// malformed

		for(size_t offset = group_offset; offset + 3 <= fi_list_end;) {
			uint16_t id_field = data[offset] << 8 | data[offset + 1];
			int rm = (data[offset + 2] >> 4) & 0x0F;
			bool continuity = data[offset + 2] & 0x08;
			size_t length_of_freq_list = data[offset + 2] & 0x07;
			offset += 3;

			size_t freq_list_end = offset + length_of_freq_list;
			if(freq_list_end > fi_list_end) {
				offset = fi_list_end;	// malformed - can't trust the rest of this FI list either
				break;
			}

			int key = FrequencyInfoKey(oe, id_field, rm);

			if(length_of_freq_list == 0) {
				// CEI ("Length of Freq list = 0"): this entry's database changed - drop whatever
				// was cached for it so it gets rebuilt from the next full (non-CEI) occurrence
				frequency_infos.erase(key);
				offset = freq_list_end;
				continue;
			}

			FIC_FREQUENCY_INFO new_info;
			new_info.other_ensemble = oe;
			new_info.id = id_field;
			new_info.continuity = continuity;

			switch(rm) {
			case FIC_FREQUENCY_INFO::DAB_ENSEMBLE:
				new_info.range = FIC_FREQUENCY_INFO::DAB_ENSEMBLE;
				for(; offset + 3 <= freq_list_end; offset += 3) {
					uint32_t combined = (uint32_t) data[offset] << 16 | data[offset + 1] << 8 | data[offset + 2];
					int control = (combined >> 19) & 0x1F;

					FIC_FI_DAB_FREQ f;
					f.geographically_adjacent = !(control & 0x01);
					f.transmission_mode_1_signalled = control & 0x02;
					f.freq_khz = (combined & 0x7FFFF) * 16.0;
					new_info.dab_freqs.push_back(f);
				}
				break;

			case FIC_FREQUENCY_INFO::FM_RDS:
				new_info.range = FIC_FREQUENCY_INFO::FM_RDS;
				for(; offset + 1 <= freq_list_end; offset += 1) {
					FIC_FI_FM_FREQ f;
					f.freq_mhz = 87.5 + data[offset] * 0.1;
					new_info.fm_freqs.push_back(f);
				}
				break;

			case FIC_FREQUENCY_INFO::DRM:
			case FIC_FREQUENCY_INFO::AMSS:
				new_info.range = (FIC_FREQUENCY_INFO::RANGE) rm;
				for(; offset + 3 <= freq_list_end; offset += 3) {
					int id_msb = data[offset];
					uint16_t freq_c = data[offset + 1] << 8 | data[offset + 2];

					FIC_FI_OTHER_FREQ f;
					f.id_msb = id_msb;
					if(rm == FIC_FREQUENCY_INFO::DRM) {
						bool multiplier_10khz = freq_c & 0x8000;
						f.freq_khz = (freq_c & 0x7FFF) * (multiplier_10khz ? 10.0 : 1.0);
					} else {
						f.freq_khz = freq_c & 0x7FFF;	// AMSS: always 1 kHz steps, top bit Rfu
					}
					new_info.other_freqs.push_back(f);
				}
				break;

			default:
				// reserved R&M value - can't interpret the frequency list, but its length is
				// still known, so just skip over it and keep parsing the rest of the FIG
				break;
			}
			offset = freq_list_end;

			FIC_FREQUENCY_INFO& current_info = frequency_infos[key];
			if(current_info != new_info) {
				current_info = new_info;

				size_t count = current_info.range == FIC_FREQUENCY_INFO::DAB_ENSEMBLE ? current_info.dab_freqs.size() :
						current_info.range == FIC_FREQUENCY_INFO::FM_RDS ? current_info.fm_freqs.size() : current_info.other_freqs.size();
				const char *range_name = current_info.range == FIC_FREQUENCY_INFO::DAB_ENSEMBLE ? "DAB" :
						current_info.range == FIC_FREQUENCY_INFO::FM_RDS ? "FM" : current_info.range == FIC_FREQUENCY_INFO::DRM ? "DRM" : "AMSS";

				fprintf(stderr, "FICDecoder: frequency info for %s Id 0x%04X (%s): %zu alternative frequenc%s\n",
						range_name, current_info.id, current_info.other_ensemble ? "other ensemble/service" : "this ensemble/service",
						count, count == 1 ? "y" : "ies");
				Log(std::string("Frequency info for ") + range_name + " Id " + StringTools::IntToHex(current_info.id, 4) +
						" (" + (current_info.other_ensemble ? "other ensemble/service" : "this ensemble/service") + "): " +
						std::to_string(count) + " alternative frequenc" + (count == 1 ? "y" : "ies"));

				observer->FICChangeFrequencyInfo(key, current_info);
			}
		}

		group_offset = fi_list_end;
	}
}

int64_t FICDecoder::OEServiceKey(bool oe, bool pd, uint32_t sid) {
	// ETSI EN 300 401 clause 8.1.10: "The database key comprises the OE and P/D flags (...) and
	// the SId field" - sid can need the full 32 bits (data services), so oe/pd go above that,
	// rather than squeezed into a plain (32-bit) int the way the other two keys are
	return ((int64_t) (oe ? 1 : 0) << 33) | ((int64_t) (pd ? 1 : 0) << 32) | (int64_t) sid;
}

void FICDecoder::ProcessFIG0_24(const uint8_t *data, size_t len, bool oe, bool pd) {
	// FIG 0/24 - OE Services (ETSI EN 300 401 clause 8.1.10)
	size_t sid_bytes = pd ? 4 : 2;

	for(size_t offset = 0; offset + sid_bytes + 1 <= len;) {
		uint32_t sid = !pd ?
				(uint32_t) (data[offset] << 8 | data[offset + 1]) :
				(uint32_t) data[offset] << 24 | data[offset + 1] << 16 | data[offset + 2] << 8 | data[offset + 3];
		offset += sid_bytes;

		int ca_id = (data[offset] >> 4) & 0x07;
		int number_of_eids = data[offset] & 0x0F;
		offset++;

		int64_t key = OEServiceKey(oe, pd, sid);

		if(number_of_eids == 0) {
			// CEI ("Number of EIds = 0"): this service's database changed - drop whatever was
			// cached for it so it gets rebuilt cleanly from the next full (non-CEI) occurrence
			oe_services.erase(key);
			continue;
		}

		if(offset + (size_t) number_of_eids * 2 > len)
			break;	// malformed

		FIC_OE_SERVICE new_entry;
		new_entry.sid = sid;
		new_entry.is_data = pd;
		new_entry.not_in_tuned_ensemble = oe;
		new_entry.ca_id = ca_id;
		for(int i = 0; i < number_of_eids; i++) {
			new_entry.eids.push_back(data[offset] << 8 | data[offset + 1]);
			offset += 2;
		}

		FIC_OE_SERVICE& current = oe_services[key];
		if(current != new_entry) {
			current = new_entry;

			std::string eids_str;
			for(int eid : current.eids) {
				if(!eids_str.empty())
					eids_str += ", ";
				eids_str += StringTools::IntToHex(eid, 4).substr(2);
			}

			fprintf(stderr, "FICDecoder: OE services: SId 0x%s %s: ensemble(s) %s\n",
					StringTools::SidToHex((int) sid).c_str(),
					current.not_in_tuned_ensemble ? "(not in tuned ensemble)" : "(in tuned ensemble)",
					eids_str.c_str());
			Log("OE services: SId " + StringTools::SidToHex((int) sid) + " " +
					(current.not_in_tuned_ensemble ? "(not in tuned ensemble)" : "(in tuned ensemble)") +
					": ensemble(s) " + eids_str);

			observer->FICChangeOEService(key, current);
		}
	}
}

void FICDecoder::ProcessFIG1(const uint8_t *data, size_t len) {
	if(len < 1) {
		fprintf(stderr, "FICDecoder: received empty FIG 1\n");
		return;
	}

	// read/skip FIG 1 header
	FIG1_HEADER header(data[0]);
	data++;
	len--;

	// ignore other ensembles
	if(header.oe)
		return;

	// check for (un)supported extension + set ID field len
	size_t len_id = -1;
	switch(header.extension) {
	case 0:	// ensemble
	case 1:	// programme service
		len_id = 2;
		break;
	case 4:	// service component
		// programme services only (P/D = 0)
		if(data[0] & 0x80)
			return;
		len_id = 3;
		break;
	default:
//		fprintf(stderr, "FICDecoder: received unsupported FIG 1/%d with %zu field bytes\n", header.extension, len);
		return;
	}

	// check length
	size_t len_calced = len_id + 16 + 2;
	if(len != len_calced) {
		fprintf(stderr, "FICDecoder: received FIG 1/%d having %zu field bytes (expected: %zu)\n", header.extension, len, len_calced);
		return;
	}

	// parse actual label data
	FIC_LABEL label;
	label.charset = header.charset;
	memcpy(label.label, data + len_id, 16);
	label.short_label_mask = data[len_id + 16] << 8 | data[len_id + 17];


	// handle extension
	switch(header.extension) {
	case 0: {	// ensemble
		uint16_t eid = data[0] << 8 | data[1];
		ProcessFIG1_0(eid, label);
		break; }
	case 1: {	// programme service
		uint16_t sid = data[0] << 8 | data[1];
		ProcessFIG1_1(sid, label);
		break; }
	case 4: {	// service component
		int scids = data[0] & 0x0F;
		uint16_t sid = data[1] << 8 | data[2];
		ProcessFIG1_4(sid, scids, label);
		break; }
	}
}

void FICDecoder::ProcessFIG1_0(uint16_t eid, const FIC_LABEL& label) {
	if(ensemble.label != label) {
		ensemble.label = label;

		std::string label_str = ConvertLabelToUTF8(label, nullptr);
		std::string short_label_str = DeriveShortLabelUTF8(label_str, label.short_label_mask);
		fprintf(stderr, "FICDecoder: EId 0x%04X: ensemble label '" "\x1B[32m" "%s" "\x1B[0m" "' ('" "\x1B[32m" "%s" "\x1B[0m" "')\n",
				eid, label_str.c_str(), short_label_str.c_str());
		Log("EId " + StringTools::IntToHex(eid, 4) + ": ensemble label '" + label_str + "' ('" + short_label_str + "')");

		UpdateEnsemble();
	}
}

void FICDecoder::ProcessFIG1_1(uint16_t sid, const FIC_LABEL& label) {
	FIC_SERVICE& service = GetService(sid);
	if(service.label != label) {
		service.label = label;

		std::string label_str = ConvertLabelToUTF8(label, nullptr);
		std::string short_label_str = DeriveShortLabelUTF8(label_str, label.short_label_mask);
		fprintf(stderr, "FICDecoder: SId 0x%04X: programme service label '" "\x1B[32m" "%s" "\x1B[0m" "' ('" "\x1B[32m" "%s" "\x1B[0m" "')\n",
				sid, label_str.c_str(), short_label_str.c_str());
		Log("SId " + StringTools::IntToHex(sid, 4) + ": programme service label '" + label_str + "' ('" + short_label_str + "')");

		UpdateService(service);
	}
}

void FICDecoder::ProcessFIG1_4(uint16_t sid, int scids, const FIC_LABEL& label) {
	// programme services only

	FIC_SERVICE& service = GetService(sid);
	FIC_LABEL& comp_label = service.comp_labels[scids];
	if(comp_label != label) {
		comp_label = label;

		std::string label_str = ConvertLabelToUTF8(label, nullptr);
		std::string short_label_str = DeriveShortLabelUTF8(label_str, label.short_label_mask);
		fprintf(stderr, "FICDecoder: SId 0x%04X, SCIdS %2d: service component label '" "\x1B[32m" "%s" "\x1B[0m" "' ('" "\x1B[32m" "%s" "\x1B[0m" "')\n",
				sid, scids, label_str.c_str(), short_label_str.c_str());
		Log("SId " + StringTools::IntToHex(sid, 4) + ", SCIdS " + std::to_string(scids) + ": service component label '" + label_str + "' ('" + short_label_str + "')");

		UpdateService(service);
	}
}

FIC_SUBCHANNEL& FICDecoder::GetSubchannel(int subchid) {
	// created automatically, if not yet existing
	return subchannels[subchid];
}

void FICDecoder::UpdateSubchannel(int subchid) {
	observer->FICChangeSubchannel(subchid, subchannels.at(subchid));

	// update services that consist of this sub-channel
	for(const fic_services_t::value_type& service : services) {
		const FIC_SERVICE& s = service.second;
		if(s.audio_comps.find(subchid) != s.audio_comps.end())
			UpdateService(s);
	}
}

FIC_SERVICE& FICDecoder::GetService(uint16_t sid) {
	FIC_SERVICE& result = services[sid];	// created, if not yet existing

	// if new service, set SID
	if(result.IsNone())
		result.sid = sid;
	return result;
}

void FICDecoder::UpdateService(const FIC_SERVICE& service) {
	// abort update, if primary component or label not yet present
	if(service.HasNoPriCompSubchid() || service.label.IsNone())
		return;

	// secondary components (if both component and definition are present)
	bool multi_comps = false;
	for(const comp_defs_t::value_type& comp_def : service.comp_defs) {
		if(comp_def.second == service.pri_comp_subchid || service.audio_comps.find(comp_def.second) == service.audio_comps.end())
			continue;
		UpdateListedService(service, comp_def.first, true);
		multi_comps = true;
	}

	// primary component
	UpdateListedService(service, LISTED_SERVICE::scids_none, multi_comps);
}

void FICDecoder::UpdateListedService(const FIC_SERVICE& service, int scids, bool multi_comps) {
	// assemble listed service
	LISTED_SERVICE ls;
	ls.sid = service.sid;
	ls.scids = scids;
	ls.label = service.label;
	ls.pty_static = service.pty_static;
	ls.pty_dynamic = service.pty_dynamic;
	ls.asu_flags = service.asu_flags;
	ls.cids = service.cids;
	ls.ecc = service.ecc;
	ls.pri_comp_subchid = service.pri_comp_subchid;
	ls.multi_comps = multi_comps;

	if(scids == LISTED_SERVICE::scids_none) {	// primary component
		ls.audio_service = service.audio_comps.at(service.pri_comp_subchid);
	} else {									// secondary component
		ls.audio_service = service.audio_comps.at(service.comp_defs.at(scids));

		// use component label, if available
		comp_labels_t::const_iterator cl_it = service.comp_labels.find(scids);
		if(cl_it != service.comp_labels.end())
			ls.label = cl_it->second;
	}
	ls.is_audio = true;
	ls.subchid = ls.audio_service.subchid;

	// use sub-channel information, if available
	fic_subchannels_t::const_iterator sc_it = subchannels.find(ls.audio_service.subchid);
	if(sc_it != subchannels.end())
		ls.subchannel = sc_it->second;

	/* check (for) Slideshow; currently only supported in X-PAD
	 * - derive the required SCIdS (if not yet known)
	 * - derive app type from UA data (if present)
	 */
	int sls_scids = scids;
	if(sls_scids == LISTED_SERVICE::scids_none) {
		for(const comp_defs_t::value_type& comp_def : service.comp_defs) {
			if(comp_def.second == ls.audio_service.subchid) {
				sls_scids = comp_def.first;
				break;
			}
		}
	}
	if(sls_scids != LISTED_SERVICE::scids_none && service.comp_sls_uas.find(sls_scids) != service.comp_sls_uas.end())
		ls.sls_app_type = GetSLSAppType(service.comp_sls_uas.at(sls_scids));

	// forward to observer
	observer->FICChangeService(ls);
}

void FICDecoder::CheckServiceTimeouts() {
	// detect services hot-removed from the ensemble: FIG 0/2 lists the complete,
	// currently valid service table on every cycle, so any service that was
	// seen before but stays absent from it for longer than service_timeout
	// is considered gone (vs. e.g. a few corrupted/missing FIBs)
	auto now = std::chrono::steady_clock::now();

	for(fic_services_t::iterator it = services.begin(); it != services.end();) {
		const FIC_SERVICE& service = it->second;

		if(service.seen_in_fig0_2 && now - service.last_seen_fig0_2 > service_timeout) {
			fprintf(stderr, "FICDecoder: SId 0x%04X: no longer listed in FIG 0/2 for %lds - removing (hot removal)\n",
					service.sid, (long) std::chrono::duration_cast<std::chrono::seconds>(service_timeout).count());
			Log("SId " + StringTools::IntToHex(service.sid, 4) + ": no longer listed in FIG 0/2 for " +
					std::to_string(std::chrono::duration_cast<std::chrono::seconds>(service_timeout).count()) + "s - removing (hot removal)");

			RemoveService(service);
			it = services.erase(it);
		} else {
			++it;
		}
	}

	PruneStaleSubchannels();
}

void FICDecoder::RemoveService(const FIC_SERVICE& service) {
	// mirror the fan-out done in UpdateService()/UpdateListedService(), so every listed
	// row (primary + secondary components) that may have been shown gets removed again
	observer->FICRemoveService(fic_service_id_t(service.sid, int(LISTED_SERVICE::scids_none)));

	for(const comp_defs_t::value_type& comp_def : service.comp_defs) {
		if(comp_def.second == service.pri_comp_subchid)
			continue;
		observer->FICRemoveService(fic_service_id_t(service.sid, comp_def.first));
	}
}

void FICDecoder::PruneStaleSubchannels() {
	// a sub-channel still present in the ensemble is re-announced in every FIG 0/1 cycle,
	// same liveness pattern as service hot-removal via FIG 0/2 (CheckServiceTimeouts) -
	// this also correctly keeps data-only sub-channels (e.g. EPG) that never belong to
	// any listed audio service, instead of dropping them as soon as they're seen
	auto now = std::chrono::steady_clock::now();

	for(fic_subchannels_t::iterator it = subchannels.begin(); it != subchannels.end();) {
		const FIC_SUBCHANNEL& sc = it->second;
		if(sc.seen_in_fig0_1 && now - sc.last_seen_fig0_1 > service_timeout) {
			observer->FICRemoveSubchannel(it->first);
			if(data_service_sids.erase(it->first))
				observer->FICRemoveDataServiceSID(it->first);
			RemoveSPIService(it->first);
			it = subchannels.erase(it);
		} else {
			++it;
		}
	}
}

int FICDecoder::GetSLSAppType(const ua_data_t& ua_data) {
	// default values, if no UA data present
	bool ca_flag = false;
	int xpad_app_type = 12;
	bool dg_flag = false;
	int dscty = 60;	// MOT

	// if UA data present, parse X-PAD data
	if(ua_data.size() >= 2) {
		ca_flag = ua_data[0] & 0x80;
		xpad_app_type = ua_data[0] & 0x1F;
		dg_flag = ua_data[1] & 0x80;
		dscty = ua_data[1] & 0x3F;
	}

	// if no CA is used, but DGs and MOT, enable Slideshow
	if(!ca_flag && !dg_flag && dscty == 60)
		return xpad_app_type;
	else
		return LISTED_SERVICE::sls_app_type_none;
}

void FICDecoder::UpdateEnsemble() {
	// abort update, if EId or label not yet present
	if(ensemble.IsNone() || ensemble.label.IsNone())
		return;

	// forward to observer
	observer->FICChangeEnsemble(ensemble);
}

std::string FICDecoder::ConvertLabelToUTF8(const FIC_LABEL& label, std::string* charset_name) {
	// no label received yet (e.g. a service just discovered via FIG 0/2, whose FIG 1/0 label
	// hasn't arrived yet) - nothing to convert, and certainly not an actual unsupported charset
	if(label.IsNone())
		return "";

	std::string result = CharsetTools::ConvertTextToUTF8(label.label, sizeof(label.label), label.charset, false, charset_name);

	// discard trailing spaces
	size_t last_pos = result.find_last_not_of(' ');
	if(last_pos != std::string::npos)
		result.resize(last_pos + 1);

	return result;
}

const size_t FICDecoder::uep_sizes[] = {
		 16,  21,  24,  29,  35,  24,  29,  35,  42,  52,  29,  35,  42,  52,  32,  42,
		 48,  58,  70,  40,  52,  58,  70,  84,  48,  58,  70,  84, 104,  58,  70,  84,
		104,  64,  84,  96, 116, 140,  80, 104, 116, 140, 168,  96, 116, 140, 168, 208,
		116, 140, 168, 208, 232, 128, 168, 192, 232, 280, 160, 208, 280, 192, 280, 416
};
const int FICDecoder::uep_pls[] = {
		5, 4, 3, 2, 1, 5, 4, 3, 2, 1, 5, 4, 3, 2, 5, 4,
		3, 2, 1, 5, 4, 3, 2, 1, 5, 4, 3, 2, 1, 5, 4, 3,
		2, 5, 4, 3, 2, 1, 5, 4, 3, 2, 1, 5, 4, 3, 2, 1,
		5, 4, 3, 2, 1, 5, 4, 3, 2, 1, 5, 4, 2, 5, 3, 1
};
const int FICDecoder::uep_bitrates[] = {
		 32,  32,  32,  32,  32,  48,  48,  48,  48,  48,  56,  56,  56,  56,  64,  64,
		 64,  64,  64,  80,  80,  80,  80,  80,  96,  96,  96,  96,  96, 112, 112, 112,
		112, 128, 128, 128, 128, 128, 160, 160, 160, 160, 160, 192, 192, 192, 192, 192,
		224, 224, 224, 224, 224, 256, 256, 256, 256, 256, 320, 320, 320, 384, 384, 384
};
const int FICDecoder::eep_a_size_factors[] = {12,  8,  6,  4};
const int FICDecoder::eep_b_size_factors[] = {27, 21, 18, 15};

const char* FICDecoder::languages_0x00_to_0x2B[] = {
		"unknown/not applicable", "Albanian", "Breton", "Catalan", "Croatian", "Welsh", "Czech", "Danish",
		"German", "English", "Spanish", "Esperanto", "Estonian", "Basque", "Faroese", "French",
		"Frisian", "Irish", "Gaelic", "Galician", "Icelandic", "Italian", "Sami", "Latin",
		"Latvian", "Luxembourgian", "Lithuanian", "Hungarian", "Maltese", "Dutch", "Norwegian", "Occitan",
		"Polish", "Portuguese", "Romanian", "Romansh", "Serbian", "Slovak", "Slovene", "Finnish",
		"Swedish", "Turkish", "Flemish", "Walloon"
};
const char* FICDecoder::languages_0x7F_downto_0x45[] = {
		"Amharic", "Arabic", "Armenian", "Assamese", "Azerbaijani", "Bambora", "Belorussian", "Bengali",
		"Bulgarian", "Burmese", "Chinese", "Chuvash", "Dari", "Fulani", "Georgian", "Greek",
		"Gujurati", "Gurani", "Hausa", "Hebrew", "Hindi", "Indonesian", "Japanese", "Kannada",
		"Kazakh", "Khmer", "Korean", "Laotian", "Macedonian", "Malagasay", "Malaysian", "Moldavian",
		"Marathi", "Ndebele", "Nepali", "Oriya", "Papiamento", "Persian", "Punjabi", "Pushtu",
		"Quechua", "Russian", "Rusyn", "Serbo-Croat", "Shona", "Sinhalese", "Somali", "Sranan Tongo",
		"Swahili", "Tadzhik", "Tamil", "Tatar", "Telugu", "Thai", "Ukranian", "Urdu",
		"Uzbek", "Vietnamese", "Zulu"
};

const char* FICDecoder::ptys_rds_0x00_to_0x1D[] = {
		"No programme type", "News", "Current Affairs", "Information",
		"Sport", "Education", "Drama", "Culture",
		"Science", "Varied", "Pop Music", "Rock Music",
		"Easy Listening Music", "Light Classical", "Serious Classical", "Other Music",
		"Weather/meteorology", "Finance/Business", "Children's programmes", "Social Affairs",
		"Religion", "Phone In", "Travel", "Leisure",
		"Jazz Music", "Country Music", "National Music", "Oldies Music",
		"Folk Music", "Documentary"
};
const char* FICDecoder::ptys_rbds_0x00_to_0x1D[] = {
		"No program type", "News", "Information", "Sports",
		"Talk", "Rock", "Classic Rock", "Adult Hits",
		"Soft Rock", "Top 40", "Country", "Oldies",
		"Soft", "Nostalgia", "Jazz", "Classical",
		"Rhythm and Blues", "Soft Rhythm and Blues", "Foreign Language", "Religious Music",
		"Religious Talk", "Personality", "Public", "College",
		"(rfu)", "(rfu)", "(rfu)", "(rfu)",
		"(rfu)", "Weather"
};

const char* FICDecoder::asu_types_0_to_10[] = {
		"Alarm", "Road Traffic flash", "Transport flash", "Warning/Service",
		"News flash", "Area weather flash", "Event announcement", "Special event",
		"Programme Information", "Sport report", "Financial report"
};

std::string FICDecoder::ConvertLanguageToString(const int value) {
	if(value >= 0x00 && value <= 0x2B)
		return languages_0x00_to_0x2B[value];
	if(value == 0x40)
		return "background sound/clean feed";
	if(value >= 0x45 && value <= 0x7F)
		return languages_0x7F_downto_0x45[0x7F - value];
	return "unknown (" + std::to_string(value) + ")";
}

std::string FICDecoder::ConvertLTOToString(const int value) {
	// just to silence recent GCC's truncation warnings
	int lto_value = value % 0x3F;

	char lto_string[7];
	snprintf(lto_string, sizeof(lto_string), "%+03d:%02d", lto_value / 2, (lto_value % 2) ? 30 : 0);
	return lto_string;
}

std::string FICDecoder::ConvertInterTableIDToString(const int value) {
	switch(value) {
	case 0x01:
		return "RDS PTY";
	case 0x02:
		return "RBDS PTY";
	default:
		return "unknown";
	}
}

std::string FICDecoder::ConvertDateTimeToString(FIC_DAB_DT utc_dt, const int lto, bool output_ms) {
	const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

	// if desired, apply LTO
	if(lto)
		utc_dt.dt.tm_min += lto * 30;

	// normalize time (apply LTO, set day of week)
	if(mktime(&utc_dt.dt) == (time_t) -1)
		throw std::runtime_error("FICDecoder: error while normalizing date/time");

	std::string result;
	char s[11];

	strftime(s, sizeof(s), "%F", &utc_dt.dt);
	result += std::string(s) + ", " + weekdays[utc_dt.dt.tm_wday] + " - ";

	if(!utc_dt.IsMsNone()) {
		// long form
		strftime(s, sizeof(s), "%T", &utc_dt.dt);
		result += s;
		if(output_ms) {
			snprintf(s, sizeof(s), ".%03d", utc_dt.ms);
			result += s;
		}
	} else {
		// short form
		strftime(s, sizeof(s), "%R", &utc_dt.dt);
		result += s;
	}

	return result;
}

std::string FICDecoder::ConvertPTYToString(const int value, const int inter_table_id) {
	switch(inter_table_id) {
	case 0x01:
		return value <= 0x1D ? ptys_rds_0x00_to_0x1D[value]  : "(not used)";
	case 0x02:
		return value <= 0x1D ? ptys_rbds_0x00_to_0x1D[value] : "(not used)";
	default:
		return "(unknown)";
	}
}

std::string FICDecoder::ConvertASuTypeToString(const int value) {
	if(value >= 0 && value <= 10)
		return asu_types_0_to_10[value];
	return "unknown (" + std::to_string(value) + ")";
}

std::string FICDecoder::ConvertUATypeToString(const int value) {
	// ETSI TS 101 756 table 16 (registered User Application types)
	switch(value) {
	case 0x002: return "MOT Slideshow";
	case 0x003: return "MOT Broadcast Web Site";
	case 0x004: return "TPEG";
	case 0x005: return "DGPS";
	case 0x006: return "TMC";
	case 0x007: return "EPG (SPI)";
	case 0x008: return "DAB Java";
	case 0x009: return "DMB";
	case 0x00a: return "IPDC services";
	case 0x00b: return "Voice applications";
	case 0x00c: return "Middleware";
	case 0x00d: return "Filecasting";
	case 0x44a: return "Journaline";
	}
	return "data service (UAtype " + StringTools::IntToHex(value, 3) + ")";
}

std::string FICDecoder::DeriveShortLabelUTF8(const std::string& long_label, uint16_t short_label_mask) {
	std::string short_label;

	for(size_t i = 0; i < long_label.length(); i++)		// consider discarded trailing spaces
		if(short_label_mask & (0x8000 >> i))
			short_label += StringTools::UTF8Substr(long_label, i, 1);

	return short_label;
}
