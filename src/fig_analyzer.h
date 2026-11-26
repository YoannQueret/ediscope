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

#ifndef FIG_ANALYZER_H_
#define FIG_ANALYZER_H_

#include <cstdint>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>


// --- FIG analysis -----------------------------------------------------------------
// Measures how often every FIG is repeated in the FIC, and checks it against the repetition rates
// of ETSI TS 103 176 V2.6.1 clause 4 and table 1 (which builds on ETSI EN 300 401 clause 6.1):
//
// - MCI (FIG 0/0, 0/1, 0/2, 0/3, 0/7, 0/14): all of it every 96 ms; FIG 0/8 and FIG 0/13: at
//   least every 1 s (clause 4 only exempts FIG 0/8 for data components, but annex F's compliant
//   example signals FIG 0/8 for every component at 1 s - followed here)
// - SI (FIG 0/5, 0/9, 0/10, 0/17, 0/18, 0/20, 0/25, 1/x, 2/x): once per second
// - FIG 0/6, 0/21, 0/24 long form (database definition): all information at least every 2 minutes
// - FIG 0/6 activation state: at least every 10 seconds
// - FIG 0/6, 0/21, 0/24 short form CEI (change event indication): once per second for 5 seconds
// - FIG 0/19, 0/26 announcement switching: start 10 times per second for 5 seconds, continuation
//   once per second, end (ASw flags = 0) 10 times per second for 2 seconds
//
// "The actual repetition rates (...) shall not fall below one third of the nominal rates", hence
// three verdicts: OK (at the nominal rate), DEGRADED (slower, but not below one third of it) and FAIL.
//
// Repetition is measured per entity - each sub-channel for FIG 0/1, each service for FIG 0/2 or
// FIG 1/1, each database key for FIG 0/6/0/21/0/24, ... - since that is how the requirements are
// phrased. Time is counted in frames (24 ms each), so network jitter doesn't affect the figures.
// Fed with the raw FIC of every frame, independently of (and next to) FICDecoder.

enum class FIGVerdict {NONE, OBSERVING, OK, DEGRADED, FAIL};

// interval statistics of a series of occurrences
struct FIGGapStats {
	uint64_t count = 0;
	double first_ms = 0;
	double last_ms = 0;
	double min_gap_ms = 0;
	double max_gap_ms = 0;
	double sum_gap_ms = 0;

	void Add(double t_ms);
	double AvgGap() const {return count > 1 ? sum_gap_ms / (count - 1) : 0;}
};

// --- snapshot, as shown by the UI -----------------------------------------------------------------
struct FIG_ANALYSIS_ENTITY {
	std::string name;
	uint64_t count;
	double since_last_ms;	// time since its last occurrence
	double min_gap_ms;		// 0 if fewer than 2 occurrences
	double avg_gap_ms;
	double max_gap_ms;		// largest interval, including the current one (since_last_ms)
	FIGVerdict verdict;
};

struct FIG_ANALYSIS_CHECK {
	std::string fig;			// e.g. "FIG 0/6"
	std::string usage;			// e.g. "Long form, database definition"
	std::string requirement;	// e.g. "all information at least every 2 minutes"
	double nominal_ms;
	std::vector<FIG_ANALYSIS_ENTITY> entities;
	double worst_gap_ms;		// over all entities
	FIGVerdict verdict;			// worst over all entities
	bool burst;					// verdicts come from the events (CEI, announcements) instead of entities
	int events;					// number of events, for a burst check
};

struct FIG_ANALYSIS_FIG {
	std::string fig;			// e.g. "FIG 0/1"
	std::string description;	// e.g. "Basic sub-channel organization"
	uint64_t count;
	uint64_t bytes;
	double rate_per_s;
	double max_gap_ms;			// between two occurrences of this FIG, whatever their content
	FIGVerdict verdict;			// worst of its checks, NONE if no requirement applies
};

struct FIG_ANALYSIS_EVENT {
	time_t time;				// wall clock, when it started
	double t_ms;				// observation time, when it started
	std::string fig;
	std::string entity;
	std::string event;			// e.g. "CEI", "Announcement start (Traffic flash)"
	std::string requirement;
	int repetitions;
	double span_ms;				// first to last repetition
	double max_gap_ms;
	bool complete;				// its window is over (else still collecting)
	FIGVerdict verdict;
	std::string note;
};

struct FIG_ANALYSIS_SNAPSHOT {
	double observed_ms = 0;
	uint64_t fibs = 0;
	uint64_t fib_crc_errors = 0;
	double fic_load = 0;		// share of the FIB bytes carrying FIGs (vs. padding)
	int services = 0;			// from FIG 0/2
	int components = 0;
	std::vector<FIG_ANALYSIS_FIG> figs;
	std::vector<FIG_ANALYSIS_CHECK> checks;
	std::vector<FIG_ANALYSIS_EVENT> events;	// most recent first
};


// --- FIGAnalyzer -----------------------------------------------------------------
class FIGAnalyzer {
private:
	enum class CheckKind {PERIODIC, BURST};

	struct Entity {
		std::string name;
		FIGGapStats gaps;
	};

	struct Check {
		std::string fig;
		std::string usage;
		std::string requirement;
		CheckKind kind;
		double nominal_ms;			// periodic: nominal interval; burst: nominal interval during the burst
		double burst_window_ms;		// burst: how long the burst shall last
		std::map<uint64_t, Entity> entities;
		int events;
		int order;			// definition order, for display
	};

	struct Fig {
		std::string description;
		FIGGapStats gaps;
		uint64_t bytes = 0;
		std::vector<std::string> check_ids;	// the checks this FIG feeds
	};

	struct Event {
		time_t time;
		double t0_ms;
		std::string check_id;
		std::string fig;
		std::string entity;
		std::string event;
		std::string requirement;
		double nominal_ms;
		double window_ms;
		FIGGapStats gaps;
		bool complete;
		std::string note;
	};

	// announcement switching state of one cluster (FIG 0/19) or OE cluster (FIG 0/26)
	struct Cluster {
		bool active = false;
		uint16_t asw = 0;
		double start_ms = 0;	// of the current announcement
		Event *start_event = nullptr;	// while its burst window is open
		Event *end_event = nullptr;
	};

	std::mutex mutex;

	double now_ms;			// observation time of the FIB being processed
	uint64_t frames;
	uint64_t fibs;
	uint64_t fib_crc_errors;
	uint64_t fig_bytes;
	std::map<std::string, Fig> figs;		// "FIG 0/1" -> stats
	std::map<std::string, Check> checks;	// check id -> check
	std::deque<Event> events;				// oldest first
	std::map<uint64_t, Event*> open_cei;	// (check, key) -> its open CEI event
	std::map<uint64_t, Cluster> clusters;	// (FIG, cluster) -> state
	std::map<uint32_t, int> service_components;	// SId -> number of components (FIG 0/2)

	void InitChecks();
	void AddCheck(const std::string& id, const std::string& fig, const std::string& usage, const std::string& requirement, CheckKind kind, double nominal_ms, double burst_window_ms = 0);
	void Occurrence(const std::string& check_id, uint64_t key, const std::string& name);
	void CEI(const std::string& check_id, uint64_t key, const std::string& name);
	void Announcement(const std::string& fig, uint64_t key, const std::string& name, uint16_t asw);
	Event& OpenEvent(const std::string& check_id, const std::string& entity, const std::string& event, double window_ms, double nominal_ms);
	void CloseExpiredEvents();

	void ProcessFIB(const uint8_t *fib);
	void ProcessFIG(int type, const uint8_t *data, size_t len);
	void ProcessFIG0(const uint8_t *data, size_t len);
	void ProcessFIG1(const uint8_t *data, size_t len);
	void ProcessFIG2(const uint8_t *data, size_t len);

	static FIGVerdict PeriodicVerdict(double max_gap_ms, bool has_closed_gap, double nominal_ms);
	static FIGVerdict EventVerdict(const Event& event);
public:
	FIGAnalyzer();

	// one frame's FIC (3 FIBs in transmission mode I/II/IV, 4 in mode III) - 24 ms of observation
	void ProcessFIC(const uint8_t *data, size_t len);
	void Reset();

	FIG_ANALYSIS_SNAPSHOT Snapshot();

	static std::string FIGDescription(int type, int ext);
	static std::string FormatInterval(double ms);
	static std::string AnnouncementTypeName(uint16_t asw);
};

#endif /* FIG_ANALYZER_H_ */
