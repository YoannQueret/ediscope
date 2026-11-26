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

#ifndef FIC_DECODER_H_
#define FIC_DECODER_H_

#include <stdio.h>
#include <stdint.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <math.h>
#include <time.h>
#include <chrono>
#include <utility>
#include <mutex>

#include "tools.h"


struct FIG0_HEADER {
	bool cn;
	bool oe;
	bool pd;
	int extension;

	FIG0_HEADER(uint8_t data) : cn(data & 0x80), oe(data & 0x40), pd(data & 0x20), extension(data & 0x1F) {}
};

struct FIG1_HEADER {
	int charset;
	bool oe;
	int extension;

	FIG1_HEADER(uint8_t data) : charset(data >> 4), oe(data & 0x08), extension(data & 0x07) {}
};

struct FIC_LABEL {
	int charset;
	uint8_t label[16];
	uint16_t short_label_mask;

	static const int charset_none = -1;
	bool IsNone() const {return charset == charset_none;}

	FIC_LABEL() : charset(charset_none), short_label_mask(0x0000) {
		memset(label, 0x00, sizeof(label));
	}

	bool operator==(const FIC_LABEL & fic_label) const {
		return charset == fic_label.charset && !memcmp(label, fic_label.label, sizeof(label)) && short_label_mask == fic_label.short_label_mask;
	}
	bool operator!=(const FIC_LABEL & fic_label) const {
		return !(*this == fic_label);
	}
};

struct FIC_SUBCHANNEL {
	size_t start;
	size_t size;
	std::string pl;
	int bitrate;
	int language;

	// liveness, as (re)announced by FIG 0/1 - used to detect a sub-channel being hot-removed,
	// independently of whether any (audio) service ever claimed it (e.g. data-only services)
	bool seen_in_fig0_1;
	std::chrono::steady_clock::time_point last_seen_fig0_1;

	static const int language_none = -1;
	bool IsNone() const {return pl.empty() && language == language_none;}

	FIC_SUBCHANNEL() : start(0), size(0), bitrate(-1), language(language_none), seen_in_fig0_1(false) {}

	bool operator==(const FIC_SUBCHANNEL & fic_subchannel) const {
		return
				start == fic_subchannel.start &&
				size == fic_subchannel.size &&
				pl == fic_subchannel.pl &&
				bitrate == fic_subchannel.bitrate &&
				language == fic_subchannel.language;
	}
	bool operator!=(const FIC_SUBCHANNEL & fic_subchannel) const {
		return !(*this == fic_subchannel);
	}
};

struct FIC_ASW_CLUSTER {
	uint16_t asw_flags;
	int subchid;

	static const int asw_flags_none = 0x0000;

	static const int subchid_none = -1;
	bool IsNone() const {return subchid == subchid_none;}

	FIC_ASW_CLUSTER() : asw_flags(asw_flags_none), subchid(subchid_none) {}

	bool operator==(const FIC_ASW_CLUSTER & fic_asw_cluster) const {
		return asw_flags == fic_asw_cluster.asw_flags && subchid == fic_asw_cluster.subchid;
	}
	bool operator!=(const FIC_ASW_CLUSTER & fic_asw_cluster) const {
		return !(*this == fic_asw_cluster);
	}
};

typedef std::map<uint8_t,FIC_ASW_CLUSTER> asw_clusters_t;

struct FIC_DAB_DT {
	struct tm dt;
	int ms;

	static const int none = -1;
	bool IsNone() const {return dt.tm_year == none;}

	static const int ms_none = -1;
	bool IsMsNone() const {return ms == ms_none;}

	FIC_DAB_DT() : ms(ms_none) {
		dt.tm_year = none;
	}

	bool operator==(const FIC_DAB_DT & fic_dab_dt) const {
		return
				ms == fic_dab_dt.ms &&
				dt.tm_sec == fic_dab_dt.dt.tm_sec &&
				dt.tm_min == fic_dab_dt.dt.tm_min &&
				dt.tm_hour == fic_dab_dt.dt.tm_hour &&
				dt.tm_mday == fic_dab_dt.dt.tm_mday &&
				dt.tm_mon == fic_dab_dt.dt.tm_mon &&
				dt.tm_year == fic_dab_dt.dt.tm_year;
	}
	bool operator!=(const FIC_DAB_DT & fic_dab_dt) const {
		return !(*this == fic_dab_dt);
	}
};

struct FIC_ENSEMBLE {
	int eid;
	bool al_flag;
	FIC_LABEL label;
	int ecc;
	int lto;
	int inter_table_id;
	asw_clusters_t asw_clusters;

	static const int eid_none = -1;
	bool IsNone() const {return eid == eid_none;}

	static const int ecc_none = -1;
	static const int lto_none = -100;
	static const int inter_table_id_none = -1;

	FIC_ENSEMBLE() :
		eid(eid_none),
		al_flag(false),
		ecc(ecc_none),
		lto(lto_none),
		inter_table_id(inter_table_id_none)
	{}

	bool operator==(const FIC_ENSEMBLE & ensemble) const {
		return
				eid == ensemble.eid &&
				al_flag == ensemble.al_flag &&
				label == ensemble.label &&
				ecc == ensemble.ecc &&
				lto == ensemble.lto &&
				inter_table_id == ensemble.inter_table_id &&
				asw_clusters == ensemble.asw_clusters;
	}
	bool operator!=(const FIC_ENSEMBLE & ensemble) const {
		return !(*this == ensemble);
	}
};

typedef std::map<int,AUDIO_SERVICE> audio_comps_t;
typedef std::map<int,int> comp_defs_t;
typedef std::map<int,FIC_LABEL> comp_labels_t;
typedef std::vector<uint8_t> ua_data_t;
typedef std::map<int,ua_data_t> comp_sls_uas_t;
typedef std::set<uint8_t> cids_t;

struct FIC_SERVICE {
	int sid;
	int pri_comp_subchid;
	FIC_LABEL label;
	int pty_static;
	int pty_dynamic;
	uint16_t asu_flags;
	cids_t cids;

	// ECC, if it differs from the ensemble's own (FIG 0/9 extended field) - ecc_none if not excepted
	int ecc;

	// components
	audio_comps_t audio_comps;		// from FIG 0/2 : SubChId -> AUDIO_SERVICE
	comp_defs_t comp_defs;			// from FIG 0/8 : SCIdS -> SubChId
	comp_labels_t comp_labels;		// from FIG 1/4 : SCIdS -> FIC_LABEL
	comp_sls_uas_t comp_sls_uas;	// from FIG 0/13: SCIdS -> UA data

	// liveness, as (re)announced by FIG 0/2 - used to detect hot removal of this service
	bool seen_in_fig0_2;
	std::chrono::steady_clock::time_point last_seen_fig0_2;

	static const int sid_none = -1;
	bool IsNone() const {return sid == sid_none;}

	static const int pri_comp_subchid_none = -1;
	bool HasNoPriCompSubchid() const {return pri_comp_subchid == pri_comp_subchid_none;}

	static const int pty_none = -1;

	static const int asu_flags_none = 0x0000;

	static const int ecc_none = -1;

	FIC_SERVICE() : sid(sid_none), pri_comp_subchid(pri_comp_subchid_none), pty_static(pty_none), pty_dynamic(pty_none), asu_flags(asu_flags_none), ecc(ecc_none), seen_in_fig0_2(false) {}
};

struct LISTED_SERVICE {
	int sid;
	int scids;
	FIC_SUBCHANNEL subchannel;
	AUDIO_SERVICE audio_service;
	FIC_LABEL label;
	int pty_static;
	int pty_dynamic;
	int sls_app_type;
	uint16_t asu_flags;
	cids_t cids;
	int ecc;	// ecc_none, unless this service's ECC differs from the ensemble's

	// true for a real, playable audio service; false for a data-only sub-channel (e.g. EPG)
	// listed purely so its CU usage is visible - subchid is its sub-channel regardless of
	// which case this is (audio_service.subchid is only ever meaningful when is_audio)
	bool is_audio;
	int subchid;

	int pri_comp_subchid;	// only used for sorting
	bool multi_comps;

	static const int sid_none = -1;
	bool IsNone() const {return sid == sid_none;}

	static const int scids_none = -1;
	bool IsPrimary() const {return scids == scids_none;}

	static const int pty_none = -1;

	static const int asu_flags_none = 0x0000;

	static const int sls_app_type_none = -1;
	bool HasSLS() const {return sls_app_type != sls_app_type_none;}

	static const int ecc_none = -1;

	LISTED_SERVICE() :
		sid(sid_none),
		scids(scids_none),
		pty_static(pty_none),
		pty_dynamic(pty_none),
		sls_app_type(sls_app_type_none),
		asu_flags(asu_flags_none),
		ecc(ecc_none),
		is_audio(true),
		subchid(AUDIO_SERVICE::subchid_none),
		pri_comp_subchid(AUDIO_SERVICE::subchid_none),
		multi_comps(false)
	{}

	bool operator<(const LISTED_SERVICE & service) const {
		if(pri_comp_subchid != service.pri_comp_subchid)
			return pri_comp_subchid < service.pri_comp_subchid;
		if(sid != service.sid)
			return sid < service.sid;
		return scids < service.scids;
	}
};


// --- FIC_LINKED_SERVICE / FIC_SERVICE_LINKING_SET -----------------------------------------------------------------
// one "linkage set" (ETSI EN 300 401 clause 8.1.15, FIG 0/6): a group of services that either
// carry the same primary service component (hard link) or are editorially related (soft link),
// possibly on DAB, FM (RDS), DRM or AMSS. Used for "service following" (see ETSI TS 103 176): if
// this ensemble/service is lost, a receiver can retune to another member of the same set
struct FIC_LINKED_SERVICE {
	// what "id" actually identifies, per the FIG 0/6 IdLQ field - never applies to the set's own
	// key_service_sid, which is always a plain DAB SId of a service in this ensemble
	enum TYPE {
		DAB_SID,		// id = 16-bit DAB SId (same country if the set is national, ecc gives the country if international)
		RDS_PI,			// id = RDS PI-code (FM alternative)
		DRM_AMSS_ID,	// id = DRM or AMSS Service Identifier, least significant 2 bytes only
		DATA_SID,		// id = 32-bit data service SId (P/D=1 linkage set - rare)
	};

	TYPE type;
	uint32_t id;
	int ecc;	// only ever set for DAB_SID in an international (ILS) set - ecc_none otherwise

	static const int ecc_none = -1;
	FIC_LINKED_SERVICE() : type(DAB_SID), id(0), ecc(ecc_none) {}

	bool operator==(const FIC_LINKED_SERVICE& o) const {return type == o.type && id == o.id && ecc == o.ecc;}
	bool operator!=(const FIC_LINKED_SERVICE& o) const {return !(*this == o);}
};

struct FIC_SERVICE_LINKING_SET {
	int lsn;				// Linkage Set Number (12 bits) - lsn_none if not yet known
	bool active;			// LA - active vs. merely potential/deactivated link
	bool hard_link;			// S/H - hard (same programme) vs. soft (related programme) link
	bool international;	// ILS - national vs. international (or DRM/AMSS) link
	bool is_data;			// P/D - a data-service linkage set (rare) rather than a programme one

	// the first service of the set, always a DAB SId carried in this very ensemble - only given
	// once, in the FIG that starts this key's database (C/N=0); sid_none if not seen yet (e.g.
	// just tuned in, mid-cycle - self-heals once that FIG's own repeat comes around)
	int key_service_sid;

	std::vector<FIC_LINKED_SERVICE> members;	// every other service in the set

	static const int lsn_none = 0;	// LSN=0 is reserved/unused by the spec
	bool IsNone() const {return lsn == lsn_none;}

	static const int sid_none = -1;

	FIC_SERVICE_LINKING_SET() :
		lsn(lsn_none),
		active(false),
		hard_link(false),
		international(false),
		is_data(false),
		key_service_sid(sid_none)
	{}

	bool operator==(const FIC_SERVICE_LINKING_SET& o) const {
		return lsn == o.lsn && active == o.active && hard_link == o.hard_link && international == o.international &&
				is_data == o.is_data && key_service_sid == o.key_service_sid && members == o.members;
	}
	bool operator!=(const FIC_SERVICE_LINKING_SET& o) const {return !(*this == o);}
};

typedef std::map<int,FIC_SERVICE_LINKING_SET> fic_service_linking_sets_t;	// key: FICDecoder::ServiceLinkingKey()


// --- FIC_FREQUENCY_INFO -----------------------------------------------------------------
// one Frequency Information database entry (ETSI EN 300 401 clause 8.1.8, FIG 0/21): every
// alternative frequency known for one DAB ensemble, FM/RDS, DRM or AMSS source, keyed by its own
// Id (EId/PI-code/DRM or AMSS Id) and Range&Modulation (R&M)
struct FIC_FI_DAB_FREQ {
	bool geographically_adjacent;
	bool transmission_mode_1_signalled;	// mode I explicitly signalled, vs. "no mode signalled"
	double freq_khz;	// centre carrier frequency: Freq a * 16 kHz

	bool operator==(const FIC_FI_DAB_FREQ& o) const {return geographically_adjacent == o.geographically_adjacent && transmission_mode_1_signalled == o.transmission_mode_1_signalled && freq_khz == o.freq_khz;}
	bool operator!=(const FIC_FI_DAB_FREQ& o) const {return !(*this == o);}
};

struct FIC_FI_FM_FREQ {
	double freq_mhz;	// 87,5 MHz + Freq b * 100 kHz

	bool operator==(const FIC_FI_FM_FREQ& o) const {return freq_mhz == o.freq_mhz;}
	bool operator!=(const FIC_FI_FM_FREQ& o) const {return !(*this == o);}
};

struct FIC_FI_OTHER_FREQ {	// DRM or AMSS
	int id_msb;			// "Id field 2" - most significant byte of the DRM/AMSS Service Identifier
	double freq_khz;

	bool operator==(const FIC_FI_OTHER_FREQ& o) const {return id_msb == o.id_msb && freq_khz == o.freq_khz;}
	bool operator!=(const FIC_FI_OTHER_FREQ& o) const {return !(*this == o);}
};

struct FIC_FREQUENCY_INFO {
	enum RANGE {
		DAB_ENSEMBLE = 0x0,
		DRM = 0x6,
		FM_RDS = 0x8,
		AMSS = 0xE,
	};

	bool other_ensemble;	// OE - applies to the tuned ensemble/a service on it (false), or another one (true)
	RANGE range;
	uint32_t id;			// EId / RDS PI-code / DRM or AMSS Id (2 LSBytes) - meaning depends on range

	// meaning depends on range: for DAB, whether continuous output across the alternative is
	// possible; for DRM/FM/AMSS, whether there's a compensating delay for DAB's own decoding delay
	bool continuity;

	std::vector<FIC_FI_DAB_FREQ> dab_freqs;		// populated iff range == DAB_ENSEMBLE
	std::vector<FIC_FI_FM_FREQ> fm_freqs;			// iff range == FM_RDS
	std::vector<FIC_FI_OTHER_FREQ> other_freqs;	// iff range == DRM or AMSS

	FIC_FREQUENCY_INFO() : other_ensemble(false), range(DAB_ENSEMBLE), id(0), continuity(false) {}

	bool operator==(const FIC_FREQUENCY_INFO& o) const {
		return other_ensemble == o.other_ensemble && range == o.range && id == o.id && continuity == o.continuity &&
				dab_freqs == o.dab_freqs && fm_freqs == o.fm_freqs && other_freqs == o.other_freqs;
	}
	bool operator!=(const FIC_FREQUENCY_INFO& o) const {return !(*this == o);}
};

typedef std::map<int,FIC_FREQUENCY_INFO> fic_frequency_infos_t;	// key: FICDecoder::FrequencyInfoKey()


// --- FIC_OE_SERVICE -----------------------------------------------------------------
// one OE Services database entry (ETSI EN 300 401 clause 8.1.10, FIG 0/24): which ensemble(s)
// carry a given service - the missing link to resolve a FIG 0/6 DAB_SID member (identified only
// by SId) to an actual EId, and from there to its frequency via FIG 0/21
struct FIC_OE_SERVICE {
	uint32_t sid;
	bool is_data;				// P/D
	bool not_in_tuned_ensemble;	// OE - whether this SId is (not) carried in the tuned ensemble
	int ca_id;					// CAId - 0 if no Access Control System is used for the service
	std::vector<int> eids;		// every ensemble known to carry this service

	FIC_OE_SERVICE() : sid(0), is_data(false), not_in_tuned_ensemble(false), ca_id(0) {}

	bool operator==(const FIC_OE_SERVICE& o) const {
		return sid == o.sid && is_data == o.is_data && not_in_tuned_ensemble == o.not_in_tuned_ensemble &&
				ca_id == o.ca_id && eids == o.eids;
	}
	bool operator!=(const FIC_OE_SERVICE& o) const {return !(*this == o);}
};

typedef std::map<int64_t,FIC_OE_SERVICE> fic_oe_services_t;	// key: FICDecoder::OEServiceKey()

typedef std::map<uint16_t, FIC_SERVICE> fic_services_t;
typedef std::map<int, FIC_SUBCHANNEL> fic_subchannels_t;

// identifies a listed service (as exposed to observers): SId + SCIdS (LISTED_SERVICE::scids_none for the primary component)
typedef std::pair<uint16_t,int> fic_service_id_t;

// --- FICDecoderObserver -----------------------------------------------------------------
class FICDecoderObserver {
public:
	virtual ~FICDecoderObserver() {}

	virtual void FICChangeEnsemble(const FIC_ENSEMBLE& /*ensemble*/) {}
	virtual void FICChangeService(const LISTED_SERVICE& /*service*/) {}
	virtual void FICRemoveService(const fic_service_id_t& /*service_id*/) {}
	virtual void FICChangeUTCDateTime(const FIC_DAB_DT& /*utc_dt*/) {}

	// every sub-channel in the ensemble (audio AND data, e.g. EPG), so CU occupancy can be
	// shown accurately even for sub-channels with no (audio) service dablin can play
	virtual void FICChangeSubchannel(int /*subchid*/, const FIC_SUBCHANNEL& /*subchannel*/) {}
	virtual void FICRemoveSubchannel(int /*subchid*/) {}

	// the (16- or 32-bit) SId of a plain data service (e.g. EPG) whose primary component is
	// "MSC stream data" (FIG 0/2, P/D=1, TMid=1) - packet-mode (TMid=3) data services would
	// additionally require FIG 0/3 SCId resolution, which isn't implemented
	virtual void FICChangeDataServiceSID(int /*subchid*/, uint32_t /*sid*/) {}
	virtual void FICRemoveDataServiceSID(int /*subchid*/) {}

	// the User Application type (FIG 0/13, P/D=1, see ETSI TS 101 756 table 16) of a data
	// service's primary component (SCIdS 0) - e.g. 0x007 "EPG" for SPI, 0x44a "Journaline", etc.
	// - so a data service can be labelled by what it actually carries, not just "(data service)"
	virtual void FICChangeDataServiceUAType(uint32_t /*sid*/, int /*ua_type*/) {}

	// a data sub-channel's primary component (SCIdS 0) has been confirmed (FIG 0/13, P/D=1,
	// UserApplicationType 0x007 "EPG", see ETSI TS 102 371) to carry an SPI service in MSC
	// packet mode, at the given packet Address (see ETSI EN 300 401 clause 5.3.2.1) - the SPI
	// data itself (MOT Directory carousel) still has to be extracted from that address. fec
	// indicates whether the sub-channel also carries the outer RS FEC scheme (FIG 0/14,
	// clause 5.3.5), which then has to be undone before the packets become parsable
	virtual void FICChangeSPIService(int /*subchid*/, int /*address*/, bool /*fec*/) {}
	virtual void FICRemoveSPIService(int /*subchid*/) {}

	// a Service linking set (FIG 0/6, ETSI EN 300 401 clause 8.1.15) was created or changed - key
	// is FICDecoder::ServiceLinkingKey(), uniquely identifying the set (its own database key: P/D
	// + S/H + ILS + LSN)
	virtual void FICChangeServiceLinking(int /*key*/, const FIC_SERVICE_LINKING_SET& /*set*/) {}

	// a Frequency Information database entry (FIG 0/21, ETSI EN 300 401 clause 8.1.8) was created
	// or changed - key is FICDecoder::FrequencyInfoKey(), uniquely identifying the entry (its own
	// database key: OE + Id + R&M)
	virtual void FICChangeFrequencyInfo(int /*key*/, const FIC_FREQUENCY_INFO& /*info*/) {}

	// an OE Services database entry (FIG 0/24, ETSI EN 300 401 clause 8.1.10) was created or
	// changed - key is FICDecoder::OEServiceKey(), uniquely identifying the entry (its own
	// database key: OE + P/D + SId)
	virtual void FICChangeOEService(int64_t /*key*/, const FIC_OE_SERVICE& /*service*/) {}

	// human-readable record of a hot change on the ensemble (EId, SId, label, short label, protection, bit rate, ...)
	virtual void FICLogMessage(const std::string& /*message*/) {}

	virtual void FICDiscardedFIB() {}
};


// --- FICDecoder -----------------------------------------------------------------
class FICDecoder {
private:
	FICDecoderObserver *observer;
	bool disable_dyn_msgs;

	void ProcessFIB(const uint8_t *data);

	void ProcessFIG0(const uint8_t *data, size_t len);
	void ProcessFIG0_0(const uint8_t *data, size_t len);
	void ProcessFIG0_1(const uint8_t *data, size_t len);
	void ProcessFIG0_2(const uint8_t *data, size_t len);
	void ProcessFIG0_2_Data(const uint8_t *data, size_t len);
	void ProcessFIG0_3(const uint8_t *data, size_t len);
	void ProcessFIG0_13_Data(const uint8_t *data, size_t len);
	void ProcessFIG0_5(const uint8_t *data, size_t len);
	void ProcessFIG0_8(const uint8_t *data, size_t len);
	void ProcessFIG0_9(const uint8_t *data, size_t len);
	void ProcessFIG0_10(const uint8_t *data, size_t len);
	void ProcessFIG0_13(const uint8_t *data, size_t len);
	void ProcessFIG0_14(const uint8_t *data, size_t len);
	void ProcessFIG0_17(const uint8_t *data, size_t len);
	void ProcessFIG0_18(const uint8_t *data, size_t len);
	void ProcessFIG0_19(const uint8_t *data, size_t len);
	void ProcessFIG0_6(const uint8_t *data, size_t len, bool pd, bool first_in_db);
	void ProcessFIG0_21(const uint8_t *data, size_t len, bool oe);
	void ProcessFIG0_24(const uint8_t *data, size_t len, bool oe, bool pd);

	void ProcessFIG1(const uint8_t *data, size_t len);
	void ProcessFIG1_0(uint16_t eid, const FIC_LABEL& label);
	void ProcessFIG1_1(uint16_t sid, const FIC_LABEL& label);
	void ProcessFIG1_4(uint16_t sid, int scids, const FIC_LABEL& label);

	FIC_SUBCHANNEL& GetSubchannel(int subchid);
	void UpdateSubchannel(int subchid);
	FIC_SERVICE& GetService(uint16_t sid);
	void UpdateService(const FIC_SERVICE& service);
	void UpdateListedService(const FIC_SERVICE& service, int scids, bool multi_comps);
	int GetSLSAppType(const ua_data_t& ua_data);

	void SetDataServiceSid(int subchid, uint32_t sid);
	void SetDataServiceUAType(uint32_t sid, int ua_type);
	void ResolvePacketDataService(int scid);
	void ResolveSPIService(uint32_t sid);
	void RemoveSPIService(int subchid);

	void Log(const std::string& message);

	FIC_ENSEMBLE ensemble;
	void UpdateEnsemble();

	fic_services_t services;
	fic_subchannels_t subchannels;	// from FIG 0/1: SubChId -> FIC_SUBCHANNEL
	std::map<int,uint32_t> data_service_sids;	// from FIG 0/2 (P/D=1): SubChId -> data service SId
	std::map<uint32_t,int> data_service_subchids;	// reverse of data_service_sids: SId -> SubChId
	std::map<int,uint32_t> packet_data_service_sids;	// from FIG 0/2 (P/D=1, TMid=3): SCId -> SId (pending)
	std::map<uint32_t,int> packet_data_service_scids;	// reverse of packet_data_service_sids: SId -> SCId
	std::map<int,int> packet_data_subchids;	// from FIG 0/3: SCId -> SubChId (pending)
	std::map<int,int> packet_data_addresses;	// from FIG 0/3: SCId -> packet Address (pending)
	std::map<uint32_t,int> data_service_ua_types;	// from FIG 0/13 (P/D=1): SId -> UAtype of its primary component (SCIdS 0)
	std::set<uint32_t> spi_service_sids;	// from FIG 0/13 (P/D=1): data service SIds whose primary component (SCIdS 0) is UAtype EPG
	std::map<int,int> spi_subchannels;	// currently confirmed SPI (FIG 0/13 UAtype EPG) sub-channels: SubChId -> packet Address
	std::map<int,bool> packet_mode_fec;	// from FIG 0/14: SubChId -> FEC scheme applied (EN 300 401 clause 5.3.5)

	fic_service_linking_sets_t service_linking_sets;	// from FIG 0/6
	fic_frequency_infos_t frequency_infos;				// from FIG 0/21
	fic_oe_services_t oe_services;						// from FIG 0/24

	// Process() runs on the EDI playback thread, CheckServiceTimeouts() on the ensemble source's
	// reader thread and Reset() on the UI thread: this serialises every access to the state above
	std::mutex mutex;

	void RemoveService(const FIC_SERVICE& service);
	void PruneStaleSubchannels();

	// minimum time a service may go unmentioned in FIG 0/2 before being considered hot-removed
	static const std::chrono::seconds service_timeout;

	FIC_DAB_DT utc_dt;
	bool utc_dt_long;

	static const size_t uep_sizes[];
	static const int uep_pls[];
	static const int uep_bitrates[];
	static const int eep_a_size_factors[];
	static const int eep_b_size_factors[];

	static const char* languages_0x00_to_0x2B[];
	static const char* languages_0x7F_downto_0x45[];

	static const char* ptys_rds_0x00_to_0x1D[];
	static const char* ptys_rbds_0x00_to_0x1D[];

	static const char* asu_types_0_to_10[];
public:
	FICDecoder(FICDecoderObserver *observer, bool disable_dyn_msgs) :
		observer(observer),
		disable_dyn_msgs(disable_dyn_msgs),
		utc_dt_long(false)
	{}

	void Process(const uint8_t *data, size_t len);
	void Reset();
	void CheckServiceTimeouts();

	// database keys for fic_service_linking_sets_t/fic_frequency_infos_t - also used by consumers
	// (e.g. the GTK Links window) to cross-reference a FIG 0/6 member against FIG 0/21 entries
	static int ServiceLinkingKey(bool pd, bool hard_link, bool international, int lsn);
	static int FrequencyInfoKey(bool oe, uint32_t id, int rm);
	static int64_t OEServiceKey(bool oe, bool pd, uint32_t sid);

	static std::string ConvertLabelToUTF8(const FIC_LABEL& label, std::string* charset_name);
	static std::string ConvertLanguageToString(const int value);
	static std::string ConvertLTOToString(const int value);
	static std::string ConvertInterTableIDToString(const int value);
	static std::string ConvertDateTimeToString(FIC_DAB_DT utc_dt, const int lto, bool output_ms);
	static std::string ConvertPTYToString(const int value, const int inter_table_id);
	static std::string ConvertASuTypeToString(const int value);
	static std::string ConvertUATypeToString(const int value);
	static std::string DeriveShortLabelUTF8(const std::string& long_label, uint16_t short_label_mask);
};



#endif /* FIC_DECODER_H_ */
