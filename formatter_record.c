/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_record.c - Stage 1: Build interval_record from raw data
 *
 * Builds the unified intermediate model (interval_record) from:
 *   - sys_snapshot (raw hardware counters)
 *   - interval_stats (aggregated statistics)
 *
 * This file contains the field table and all getter functions.
 * Serializers (text/json/csv) consume interval_record without knowing
 * about show_* flags or how data is retrieved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>

#include "formatter.h"
#include "aggregator.h"
#include "pmu.h"
#include "topology.h"
#include "cpu_inventory.h"
#include "cpuidle.h"
#include "collector.h"
#include "power.h"

/* ============================================================================
 * SECTION 1: COLUMN VISIBILITY FLAGS
 * ============================================================================ */

/* Column visibility flags - exposed to main for -s/-H options */
int show_cpu = 1;
int show_freq = 1;
int show_idle = 1;
int show_iowait = 0;
int show_power = 1;
int show_temp = 1;
int show_pmu = 0;
int show_sysstat = 0;
int show_membw = 0;
int show_package = 0;
int show_core = 0;
int show_numa = 0;
int show_ipc = 0;
int show_energy = 0;

#define ARRAY_SIZE(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

/* Idle state visibility - dynamically set based on actual state count */
static int show_idle_state[8] = {1, 1, 0, 0, 0, 0, 0, 0};

/* Summary idle state visibility - same as CPU idle but for SUM row */
static int show_summary_idle_state[8] = {1, 1, 0, 0, 0, 0, 0, 0};

/*
 * Idle state override bitmasks:
 *   idle_show_mask: bit set → explicitly shown
 *   idle_hide_mask: bit set → explicitly hidden
 *   both clear    → inherit (available → visible)
 *
 * If any show bit is set, whitelist semantics apply: only explicitly
 * shown states are visible.
 */
static unsigned int idle_show_mask;
static unsigned int idle_hide_mask;

/* Idle state labels - updated from cpuidle state names when available */
static char idle_state_labels[8][16] = {
	"LPI-0", "LPI-1", "LPI-2", "LPI-3",
	"LPI-4", "LPI-5", "LPI-6", "LPI-7"
};
static char idle_state_wakeup_labels[8][24] = {
	"LPI-0_wake", "LPI-1_wake", "LPI-2_wake", "LPI-3_wake",
	"LPI-4_wake", "LPI-5_wake", "LPI-6_wake", "LPI-7_wake"
};

/* Summary temperature visibility - enabled based on discovered NUMA temp sensors */
static int show_temp_vdie[4] = {1, 1, 0, 0};


/* Memory pool for interval_record */
static struct interval_record *rec_pool;
static struct cpu_row *cpu_rows_pool;
static int cpu_rows_pool_size;
static int pool_initialized;

static void set_default_idle_state_labels(void)
{
	for (int i = 0; i < ARRAY_SIZE(idle_state_labels); i++) {
		snprintf(idle_state_labels[i], sizeof(idle_state_labels[i]), "LPI-%d", i);
		snprintf(idle_state_wakeup_labels[i],
			 sizeof(idle_state_wakeup_labels[i]), "LPI-%d_wake", i);
	}
}

static void set_idle_state_columns_visible(int count)
{
	for (int i = 0; i < ARRAY_SIZE(show_idle_state); i++) {
		int enabled = (i < count);
		show_idle_state[i] = enabled;
		show_summary_idle_state[i] = enabled;
	}
}

static void clear_idle_state_columns(void)
{
	set_idle_state_columns_visible(0);
}

static void reset_idle_state_overrides_internal(void)
{
	idle_show_mask = 0;
	idle_hide_mask = 0;
}

void clear_idle_state_overrides(void)
{
	reset_idle_state_overrides_internal();
}

void set_idle_state_override(int state_idx, int enable, int whitelist)
{
	unsigned int bit;

	(void)whitelist;
	if (state_idx < 0 || state_idx >= 8)
		return;

	bit = 1U << state_idx;
	if (enable) {
		idle_show_mask |= bit;
		idle_hide_mask &= ~bit;
	} else {
		idle_hide_mask |= bit;
		idle_show_mask &= ~bit;
	}
}

/* ============================================================================
 * SECTION 2: HELPER FUNCTIONS
 * ============================================================================ */

static int get_tracked_cpu_id(int tracked_idx)
{
	return get_cpu_id_by_tracked_idx(tracked_idx);
}

static const struct cpu_freq_info *get_cpu_freq_entry(
	const struct interval_record *rec,
	int cpu_idx)
{
	if (!rec || !rec->raw || !rec->raw->freqs || cpu_idx < 0)
		return NULL;

	return &rec->raw->freqs[cpu_idx];
}

static double clamp_percent(double pct)
{
	if (pct < 0.0)
		return 0.0;
	if (pct > 100.0)
		return 100.0;
	return pct;
}

static const struct idle_state *get_cpu_idle_state_entry(
	const struct interval_record *rec,
	int cpu_idx,
	int state_idx)
{
	int cpu_id;
	int state_count;

	if (!rec || !rec->raw || !rec->raw->idle || cpu_idx < 0)
		return NULL;

	state_count = rec->raw->idle_state_count;
	if (state_idx < 0 || state_idx >= state_count)
		return NULL;

	cpu_id = get_tracked_cpu_id(cpu_idx);
	if (cpu_id < 0 || !rec->raw->idle[cpu_id])
		return NULL;

	return &rec->raw->idle[cpu_id][state_idx];
}

static const struct idle_state *get_usable_idle_state_entry(
	const struct interval_record *rec,
	int cpu_idx,
	int state_idx)
{
	const struct idle_state *state =
		get_cpu_idle_state_entry(rec, cpu_idx, state_idx);

	if (!state || !state->available || state->disabled)
		return NULL;

	return state;
}

static int idle_state_is_visible(int state_idx, int summary_mode)
{
	if (state_idx < 0 || state_idx >= ARRAY_SIZE(show_idle_state))
		return 0;

	return summary_mode ? show_summary_idle_state[state_idx] :
			      show_idle_state[state_idx];
}

static const struct idle_state *get_visible_usable_idle_state_entry(
	const struct interval_record *rec,
	int cpu_idx,
	int state_idx,
	int summary_mode)
{
	if (!idle_state_is_visible(state_idx, summary_mode))
		return NULL;

	return get_usable_idle_state_entry(rec, cpu_idx, state_idx);
}

static int get_last_visible_usable_idle_state(const struct interval_record *rec,
					      int cpu_idx, int summary_mode)
{
	int state_count;

	if (!rec || !rec->raw)
		return -1;

	state_count = rec->raw->idle_state_count;
	if (state_count > ARRAY_SIZE(show_idle_state))
		state_count = ARRAY_SIZE(show_idle_state);

	for (int state_idx = state_count - 1; state_idx >= 0; state_idx--) {
		if (get_visible_usable_idle_state_entry(rec, cpu_idx, state_idx,
							summary_mode))
			return state_idx;
	}

	return -1;
}

/*
 * LPI display rule:
 *   - Busy/Idle comes from /proc/stat and is treated as authoritative.
 *   - Shallow states keep their raw cpuidle residency percentages.
 *   - The deepest usable state becomes the residual bucket so that
 *     sum(LPI-*) is forced to match Idle% for each CPU row.
 *
 * This keeps shallow state visibility intuitive on ARM while avoiding
 * short-interval drift where raw cpuidle state totals undercount idle.
 */
static double get_cpu_idle_state_percent_internal(const struct interval_record *rec,
						  int cpu_idx, int state_idx,
						  int summary_mode)
{
	const struct idle_state *state;
	int last_state;
	double idle_pct;
	double remaining;

	state = get_visible_usable_idle_state_entry(rec, cpu_idx, state_idx,
						   summary_mode);
	if (!state)
		return summary_mode ? 0.0 : NAN;

	if (!rec->stats || cpu_idx >= rec->cpu_count)
		return summary_mode ? 0.0 : NAN;

	last_state = get_last_visible_usable_idle_state(rec, cpu_idx, summary_mode);
	if (last_state < 0)
		return summary_mode ? 0.0 : NAN;

	idle_pct = clamp_percent(rec->stats->per_cpu_idle[cpu_idx]);

	/*
	 * Preserve shallower states first and let the deepest usable state absorb
	 * the residual. This guarantees sum(LPI-*) == Idle% on each CPU.
	 */
	remaining = idle_pct;
	for (int idx = 0; idx <= last_state; idx++) {
		const struct idle_state *iter_state;
		double raw_pct;
		double displayed_pct;

		iter_state = get_visible_usable_idle_state_entry(rec, cpu_idx, idx,
								 summary_mode);
		if (!iter_state)
			continue;

		if (idx == last_state)
			displayed_pct = remaining;
		else {
			raw_pct = clamp_percent(iter_state->percentage);
			displayed_pct = raw_pct;
			if (displayed_pct > remaining)
				displayed_pct = remaining;
			remaining -= displayed_pct;
			if (remaining < 0.0)
				remaining = 0.0;
		}

		if (idx == state_idx)
			return clamp_percent(displayed_pct);
	}

	return summary_mode ? 0.0 : NAN;
}

static double get_cpu_idle_state_percent(const struct interval_record *rec,
					 int cpu_idx, int state_idx)
{
	return get_cpu_idle_state_percent_internal(rec, cpu_idx, state_idx, 0);
}

static double get_summary_idle_state_percent(const struct interval_record *rec,
					     int state_idx)
{
	double total = 0.0;
	int cpu_count;

	if (!rec)
		return 0.0;

	cpu_count = rec->cpu_count;
	if (cpu_count <= 0)
		return 0.0;

	for (int tracked_idx = 0; tracked_idx < cpu_count; tracked_idx++)
		total += get_cpu_idle_state_percent_internal(rec, tracked_idx,
							      state_idx, 1);

	return total / cpu_count;
}

/* ============================================================================
 * SECTION 3: FIELD GETTER FUNCTIONS
 * ============================================================================ */

/*
 * Get CPU package ID
 */
static int get_cpu_package(const struct interval_record *rec, int cpu_idx)
{
	(void)rec;
	int cpu_id = get_tracked_cpu_id(cpu_idx);
	return get_package_id(cpu_id);
}

/*
 * Get CPU core ID
 */
static int get_cpu_core(const struct interval_record *rec, int cpu_idx)
{
	(void)rec;
	int cpu_id = get_tracked_cpu_id(cpu_idx);
	return get_core_id(cpu_id);
}

/*
 * Get CPU NUMA node
 */
static int get_cpu_numa_node(const struct interval_record *rec, int cpu_idx)
{
	(void)rec;
	int cpu_id = get_tracked_cpu_id(cpu_idx);
	return get_numa_node(cpu_id);
}

/* --- Frequency getters --- */

static double get_cpu_freq_mhz(const struct interval_record *rec, int cpu_idx)
{
	const struct cpu_freq_info *freq = get_cpu_freq_entry(rec, cpu_idx);

	if (!freq)
		return 0;
	return freq->cur_freq / 1000.0;
}

static double get_cpu_min_freq_mhz(const struct interval_record *rec, int cpu_idx)
{
	const struct cpu_freq_info *freq = get_cpu_freq_entry(rec, cpu_idx);

	if (!freq)
		return 0;
	return freq->min_freq / 1000.0;
}

static double get_cpu_max_freq_mhz(const struct interval_record *rec, int cpu_idx)
{
	const struct cpu_freq_info *freq = get_cpu_freq_entry(rec, cpu_idx);

	if (!freq)
		return 0;
	return freq->max_freq / 1000.0;
}

static const char *get_cpu_governor(const struct interval_record *rec, int cpu_idx)
{
	const struct cpu_freq_info *freq = get_cpu_freq_entry(rec, cpu_idx);

	if (!freq)
		return "";
	return freq->governor;
}

static const char *get_cpu_boost(const struct interval_record *rec, int cpu_idx)
{
	static const char *const unavailable = "-";
	static const char *const disabled = "0";
	static const char *const enabled = "1";
	const struct cpu_freq_info *freq = get_cpu_freq_entry(rec, cpu_idx);

	if (!freq)
		return unavailable;
	if (freq->boost < 0)
		return unavailable;

	return freq->boost ? enabled : disabled;
}

/* --- Idle/busy getters --- */

static double get_cpu_busy_percent(const struct interval_record *rec, int cpu_idx)
{
	if (!rec || !rec->stats)
		return 0;
	return 100.0 - rec->stats->per_cpu_idle[cpu_idx];
}

static double get_cpu_idle_percent(const struct interval_record *rec, int cpu_idx)
{
	if (!rec || !rec->stats)
		return 0;
	return rec->stats->per_cpu_idle[cpu_idx];
}

static double get_cpu_iowait_percent(const struct interval_record *rec, int cpu_idx)
{
	if (!rec || !rec->stats)
		return 0;
	return rec->stats->per_cpu_iowait[cpu_idx];
}

static double get_cpu_ipc(const struct interval_record *rec, int cpu_idx)
{
	if (!rec || !rec->stats || cpu_idx < 0)
		return NAN;
	if (!pmu_is_active())
		return NAN;
	return rec->stats->per_cpu_ipc[cpu_idx];
}

/* --- Per-idle-state getters --- */

#define DEFINE_CPU_IDLE_STATE_GETTER(state_idx)					\
static double get_cpu_idle_state##state_idx(const struct interval_record *rec,	\
					    int cpu_idx)			\
{										\
	return get_cpu_idle_state_percent(rec, cpu_idx, state_idx);		\
}

DEFINE_CPU_IDLE_STATE_GETTER(0)
DEFINE_CPU_IDLE_STATE_GETTER(1)
DEFINE_CPU_IDLE_STATE_GETTER(2)
DEFINE_CPU_IDLE_STATE_GETTER(3)
DEFINE_CPU_IDLE_STATE_GETTER(4)
DEFINE_CPU_IDLE_STATE_GETTER(5)
DEFINE_CPU_IDLE_STATE_GETTER(6)
DEFINE_CPU_IDLE_STATE_GETTER(7)

/* --- Idle-state wakeup getters --- */

#define DEFINE_CPU_IDLE_STATE_WAKEUP_GETTER(state_idx)				\
static double get_cpu_idle_state_wakeup##state_idx(const struct interval_record *rec, \
					    int cpu_idx)			\
{										\
	const struct idle_state *state =					\
		get_usable_idle_state_entry(rec, cpu_idx, state_idx);		\
	if (!state)								\
		return 0;							\
	return state->wakeups_per_sec;						\
}

DEFINE_CPU_IDLE_STATE_WAKEUP_GETTER(0)
DEFINE_CPU_IDLE_STATE_WAKEUP_GETTER(1)
DEFINE_CPU_IDLE_STATE_WAKEUP_GETTER(2)
DEFINE_CPU_IDLE_STATE_WAKEUP_GETTER(3)
DEFINE_CPU_IDLE_STATE_WAKEUP_GETTER(4)
DEFINE_CPU_IDLE_STATE_WAKEUP_GETTER(5)
DEFINE_CPU_IDLE_STATE_WAKEUP_GETTER(6)
DEFINE_CPU_IDLE_STATE_WAKEUP_GETTER(7)

/* --- Temperature getters --- */

/*
 * Get per-CPU temperature based on NUMA node
 * CPU belongs to NUMA 0 -> show vdie0, NUMA 1 -> show vdie1
 */
static double get_cpu_temp_c(const struct interval_record *rec, int cpu_idx)
{
	int cpu_id;

	if (!rec || !rec->raw)
		return 0;

	cpu_id = get_tracked_cpu_id(cpu_idx);
	if (cpu_id < 0)
		return 0;

	/* Get NUMA node for this CPU */
	int numa = get_numa_node(cpu_id);
	if (numa < 0 || numa >= rec->raw->numa_temp_count)
		return 0;

	/* Return temperature for this NUMA node */
	return rec->raw->numa_temps[numa] / 1000.0;
}

/* NUMA temperature getters for SUM level */
static double get_temp_vdie_by_numa(const struct interval_record *rec, int numa)
{
	if (!rec || !rec->raw)
		return 0;
	if (numa < 0 || numa >= rec->raw->numa_temp_count)
		return 0;
	return rec->raw->numa_temps[numa] / 1000.0;
}

#define DEFINE_NUMA_TEMP_GETTER(numa_idx)					\
static double get_temp_vdie##numa_idx(const struct interval_record *rec, int cpu)	\
{										\
	(void)cpu;								\
	return get_temp_vdie_by_numa(rec, numa_idx);				\
}

DEFINE_NUMA_TEMP_GETTER(0)
DEFINE_NUMA_TEMP_GETTER(1)
DEFINE_NUMA_TEMP_GETTER(2)
DEFINE_NUMA_TEMP_GETTER(3)

/* --- Package getters --- */

static const struct package_row *get_package_row(
	const struct interval_record *rec,
	int pkg_idx)
{
	if (!rec || !rec->stats ||
	    pkg_idx < 0 || pkg_idx >= rec->stats->package_count)
		return NULL;

	return &rec->stats->packages[pkg_idx];
}

static int get_pkg_package_id(const struct interval_record *rec, int cpu)
{
	/* cpu parameter is actually package index for package-scope fields */
	const struct package_row *pkg = get_package_row(rec, cpu);
	return pkg ? pkg->package_id : 0;
}

static double get_pkg_avg_mhz(const struct interval_record *rec, int cpu)
{
	const struct package_row *pkg = get_package_row(rec, cpu);
	return pkg ? pkg->avg_mhz : 0;
}

static double get_pkg_idle_percent(const struct interval_record *rec, int cpu)
{
	const struct package_row *pkg = get_package_row(rec, cpu);
	return pkg ? pkg->idle_percent : 0;
}

static double get_pkg_busy_percent(const struct interval_record *rec, int cpu)
{
	const struct package_row *pkg = get_package_row(rec, cpu);
	return pkg ? pkg->busy_percent : 0;
}

static double get_pkg_iowait_percent(const struct interval_record *rec, int cpu)
{
	const struct package_row *pkg = get_package_row(rec, cpu);
	return pkg ? pkg->iowait_percent : 0;
}

static int get_pkg_cpu_count(const struct interval_record *rec, int cpu)
{
	const struct package_row *pkg = get_package_row(rec, cpu);
	return pkg ? pkg->cpu_count : 0;
}

/* --- Summary getters --- */

static double get_summary_avg_mhz(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? rec->summary.avg_mhz : 0;
}

static double get_summary_uncore_freq_mhz(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? rec->summary.uncore_freq_mhz : 0;
}

static double get_summary_busy_percent(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? rec->summary.busy_percent : 0;
}

static double get_summary_idle_percent(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? rec->summary.idle_percent : 0;
}

static double get_summary_iowait_percent(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? rec->summary.iowait_percent : 0;
}

#define DEFINE_SUMMARY_IDLE_STATE_GETTER(state_idx)				\
static double get_summary_idle_state##state_idx(const struct interval_record *rec,	\
						int cpu)			\
{										\
	(void)cpu;								\
	return get_summary_idle_state_percent(rec, state_idx);			\
}

DEFINE_SUMMARY_IDLE_STATE_GETTER(0)
DEFINE_SUMMARY_IDLE_STATE_GETTER(1)
DEFINE_SUMMARY_IDLE_STATE_GETTER(2)
DEFINE_SUMMARY_IDLE_STATE_GETTER(3)
DEFINE_SUMMARY_IDLE_STATE_GETTER(4)
DEFINE_SUMMARY_IDLE_STATE_GETTER(5)
DEFINE_SUMMARY_IDLE_STATE_GETTER(6)
DEFINE_SUMMARY_IDLE_STATE_GETTER(7)

static long long get_summary_power_mw(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? rec->summary.power_mw : 0;
}

static double get_summary_energy_joules(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? rec->summary.energy_joules : 0;
}

static long long get_summary_mem_bw(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? (long long)rec->summary.mem_bw : 0;
}

static long long get_summary_ctx_switches(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? (long long)rec->summary.ctx_switches : 0;
}

static long long get_summary_interrupts(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? (long long)rec->summary.interrupts : 0;
}

static long long get_summary_soft_interrupts(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	return rec ? (long long)rec->summary.soft_interrupts : 0;
}

static double get_summary_ipc(const struct interval_record *rec, int cpu)
{
	(void)cpu;
	if (!rec)
		return NAN;
	if (!pmu_is_active())
		return NAN;
	return rec->summary.ipc;
}

/* ============================================================================
 * SECTION 4: FIELD TABLE
 * ============================================================================ */

#define SUMMARY_IDLE_FIELD(idx)							\
	{"sum_idle_state" #idx, idle_state_labels[idx], "lpi" #idx,		\
	 FIELD_SCOPE_SYSTEM, FIELD_TYPE_DOUBLE, FIELD_GROUP_IDLE,		\
	 FIELD_SERIES_IDLE_STATE, idx,					\
	 &show_summary_idle_state[idx],					\
	 .getter.get_double = get_summary_idle_state##idx}

#define TEMP_VDIE_FIELD(idx)							\
	{"temp_vdie" #idx, "Temp" #idx, "temp" #idx,				\
	 FIELD_SCOPE_SYSTEM, FIELD_TYPE_DOUBLE, FIELD_GROUP_TEMP,		\
	 FIELD_SERIES_SUMMARY_TEMP, idx,					\
	 &show_temp_vdie[idx],						\
	 .getter.get_double = get_temp_vdie##idx}

#define CPU_IDLE_WAKEUP_FIELD(idx)						\
	{"idle_state_wakeup" #idx, idle_state_wakeup_labels[idx], "lpi" #idx "_wake", \
	 FIELD_SCOPE_CPU, FIELD_TYPE_DOUBLE, FIELD_GROUP_IDLE,		\
	 FIELD_SERIES_IDLE_STATE, idx,					\
	 &show_idle_state[idx],						\
	 .getter.get_double = get_cpu_idle_state_wakeup##idx}

#define CPU_IDLE_FIELD(idx)							\
	{"idle_state" #idx, idle_state_labels[idx], "lpi" #idx,			\
	 FIELD_SCOPE_CPU, FIELD_TYPE_DOUBLE, FIELD_GROUP_IDLE,		\
	 FIELD_SERIES_IDLE_STATE, idx,					\
	 &show_idle_state[idx],						\
	 .getter.get_double = get_cpu_idle_state##idx}

#define SYSTEM_DOUBLE_FIELD(id, label, json_label, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, FIELD_SCOPE_SYSTEM, FIELD_TYPE_DOUBLE,	\
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_double = getter_fn }

#define SYSTEM_LLONG_FIELD(id, label, json_label, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, FIELD_SCOPE_SYSTEM, FIELD_TYPE_LLONG,	\
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_llong = getter_fn }

#define CPU_INT_FIELD(id, label, json_label, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, FIELD_SCOPE_CPU, FIELD_TYPE_INT,		\
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_int = getter_fn }

#define CPU_DOUBLE_FIELD(id, label, json_label, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, FIELD_SCOPE_CPU, FIELD_TYPE_DOUBLE,	\
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_double = getter_fn }

#define CPU_STRING_FIELD(id, label, json_label, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, FIELD_SCOPE_CPU, FIELD_TYPE_STRING,		\
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_string = getter_fn }

/* ============================================================================
 * SECTION 4: FIELD DESCRIPTOR TABLE
 * ============================================================================ */

struct field_desc all_fields[] = {
	
#define PACKAGE_DOUBLE_FIELD(id, label, json_label, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, FIELD_SCOPE_PACKAGE, FIELD_TYPE_DOUBLE,	\
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_double = getter_fn }

#define PACKAGE_INT_FIELD(id, label, json_label, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, FIELD_SCOPE_PACKAGE, FIELD_TYPE_INT,		\
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_int = getter_fn }

	/* Per-package fields */
	PACKAGE_INT_FIELD("pkg_id", "Pkg", "package_id", FIELD_GROUP_PACKAGE,
			  &show_package, get_pkg_package_id),
	PACKAGE_DOUBLE_FIELD("pkg_avg_freq", "Freq", "freq", FIELD_GROUP_FREQ,
			 &show_freq, get_pkg_avg_mhz),
	PACKAGE_DOUBLE_FIELD("pkg_idle_percent", "Idle%", "idle_percent", FIELD_GROUP_IDLE,
			 &show_idle, get_pkg_idle_percent),
	PACKAGE_DOUBLE_FIELD("pkg_busy_percent", "Busy%", "busy_percent", FIELD_GROUP_IDLE,
			 &show_idle, get_pkg_busy_percent),
	PACKAGE_DOUBLE_FIELD("pkg_iowait_percent", "IOWait%", "iowait_percent", FIELD_GROUP_IDLE,
			 &show_iowait, get_pkg_iowait_percent),
	PACKAGE_INT_FIELD("pkg_cpu_count", "CPUs", "cpu_count", FIELD_GROUP_PACKAGE,
		      &show_package, get_pkg_cpu_count),

	/* System-wide fields (shown in SUM row) */
	SYSTEM_DOUBLE_FIELD("avg_mhz", "AvgFreq", "avg_freq", FIELD_GROUP_FREQ,
			    &show_freq, get_summary_avg_mhz),
	SYSTEM_DOUBLE_FIELD("uncore_freq", "UncoreFreq", "uncore_freq", FIELD_GROUP_FREQ,
			    &show_freq, get_summary_uncore_freq_mhz),
	/* Summary idle state percentages - show before busy%/idle% */
	SUMMARY_IDLE_FIELD(0),
	SUMMARY_IDLE_FIELD(1),
	SUMMARY_IDLE_FIELD(2),
	SUMMARY_IDLE_FIELD(3),
	SUMMARY_IDLE_FIELD(4),
	SUMMARY_IDLE_FIELD(5),
	SUMMARY_IDLE_FIELD(6),
	SUMMARY_IDLE_FIELD(7),
	SYSTEM_DOUBLE_FIELD("idle_percent", "Idle%", "idle_percent", FIELD_GROUP_IDLE,
			    &show_idle, get_summary_idle_percent),
	SYSTEM_DOUBLE_FIELD("iowait_percent", "IOWait%", "iowait_percent", FIELD_GROUP_IDLE,
			    &show_iowait, get_summary_iowait_percent),
	SYSTEM_DOUBLE_FIELD("busy_percent", "Busy%", "busy_percent", FIELD_GROUP_IDLE,
			    &show_idle, get_summary_busy_percent),
	SYSTEM_LLONG_FIELD("power_mw", "Power", "power", FIELD_GROUP_POWER,
			   &show_power, get_summary_power_mw),
	TEMP_VDIE_FIELD(0),
	TEMP_VDIE_FIELD(1),
	TEMP_VDIE_FIELD(2),
	TEMP_VDIE_FIELD(3),
	SYSTEM_DOUBLE_FIELD("energy_joules", "Energy", "energy", FIELD_GROUP_ENERGY,
			    &show_energy, get_summary_energy_joules),
	SYSTEM_LLONG_FIELD("mem_bw", "MemBW", "mem_bw", FIELD_GROUP_MEMBW,
			   &show_membw, get_summary_mem_bw),
	SYSTEM_LLONG_FIELD("ctx_switches", "CtxSw", "ctx_switches", FIELD_GROUP_SYSSTAT,
			   &show_sysstat, get_summary_ctx_switches),
	SYSTEM_LLONG_FIELD("interrupts", "IRQs", "interrupts", FIELD_GROUP_SYSSTAT,
			   &show_sysstat, get_summary_interrupts),
	SYSTEM_LLONG_FIELD("soft_interrupts", "SoftIRQs", "soft_interrupts", FIELD_GROUP_SYSSTAT,
			   &show_sysstat, get_summary_soft_interrupts),
	SYSTEM_DOUBLE_FIELD("ipc", "IPC", "ipc", FIELD_GROUP_IPC,
			    &show_ipc, get_summary_ipc),

	/* Per-CPU fields (excluding 'cpu' which is printed manually in serializers) */
	CPU_INT_FIELD("package", "Pkg", "package", FIELD_GROUP_PACKAGE,
		      &show_package, get_cpu_package),
	CPU_INT_FIELD("core", "Core", "core", FIELD_GROUP_CORE,
		      &show_core, get_cpu_core),
	CPU_INT_FIELD("numa_node", "Node", "node", FIELD_GROUP_NUMA,
		      &show_numa, get_cpu_numa_node),
	CPU_DOUBLE_FIELD("freq_mhz", "Freq", "freq", FIELD_GROUP_FREQ,
			 &show_freq, get_cpu_freq_mhz),
	CPU_DOUBLE_FIELD("min_freq_mhz", "Min", "min", FIELD_GROUP_FREQ,
			 &show_freq, get_cpu_min_freq_mhz),
	CPU_DOUBLE_FIELD("max_freq_mhz", "Max", "max", FIELD_GROUP_FREQ,
			 &show_freq, get_cpu_max_freq_mhz),
	CPU_STRING_FIELD("governor", "Governor", "governor", FIELD_GROUP_FREQ,
			 &show_freq, get_cpu_governor),
	CPU_STRING_FIELD("boost", "Boost", "boost", FIELD_GROUP_FREQ,
			 &show_freq, get_cpu_boost),
	/* Per-idle-state percentages - show before busy%/idle% */
	CPU_IDLE_FIELD(0),
	CPU_IDLE_FIELD(1),
	CPU_IDLE_FIELD(2),
	CPU_IDLE_FIELD(3),
	CPU_IDLE_FIELD(4),
	CPU_IDLE_FIELD(5),
	CPU_IDLE_FIELD(6),
	CPU_IDLE_FIELD(7),
	CPU_IDLE_WAKEUP_FIELD(0),
	CPU_IDLE_WAKEUP_FIELD(1),
	CPU_IDLE_WAKEUP_FIELD(2),
	CPU_IDLE_WAKEUP_FIELD(3),
	CPU_IDLE_WAKEUP_FIELD(4),
	CPU_IDLE_WAKEUP_FIELD(5),
	CPU_IDLE_WAKEUP_FIELD(6),
	CPU_IDLE_WAKEUP_FIELD(7),
	CPU_DOUBLE_FIELD("cpu_idle_percent", "Idle%", "idle_percent", FIELD_GROUP_IDLE,
			 &show_idle, get_cpu_idle_percent),
	CPU_DOUBLE_FIELD("cpu_iowait_percent", "IOWait%", "iowait_percent", FIELD_GROUP_IDLE,
			 &show_iowait, get_cpu_iowait_percent),
	CPU_DOUBLE_FIELD("cpu_busy_percent", "Busy%", "busy_percent", FIELD_GROUP_IDLE,
			 &show_idle, get_cpu_busy_percent),
	CPU_DOUBLE_FIELD("cpu_ipc", "IPC", "ipc", FIELD_GROUP_IPC,
			 &show_ipc, get_cpu_ipc),
	/* Note: cpu_power_mw removed - power only shown at SUM level */
	CPU_DOUBLE_FIELD("cpu_temp_c", "Temp", "temp", FIELD_GROUP_TEMP,
			 &show_temp, get_cpu_temp_c),
};

#define NUM_FIELDS (int)(sizeof(all_fields) / sizeof(all_fields[0]))
_Static_assert(NUM_FIELDS <= 64,
	       "NUM_FIELDS exceeds uint64_t bitmask capacity");

/*
 * Field override bitmasks.
 *
 * field_show_mask: bit set → explicitly show this field
 * field_hide_mask: bit set → explicitly hide this field
 * both clear: inherit from group flag (show_* variable)
 *
 * If any show bit is set, whitelist semantics apply: only fields with
 * their show bit set are visible.
 */
static uint64_t field_show_mask;
static uint64_t field_hide_mask;

#undef SUMMARY_IDLE_FIELD
#undef TEMP_VDIE_FIELD
#undef CPU_IDLE_WAKEUP_FIELD
#undef CPU_IDLE_FIELD
#undef SYSTEM_DOUBLE_FIELD
#undef SYSTEM_LLONG_FIELD
#undef CPU_INT_FIELD
#undef CPU_DOUBLE_FIELD
#undef PACKAGE_DOUBLE_FIELD
#undef PACKAGE_INT_FIELD
#undef CPU_STRING_FIELD

static int field_is_effectively_enabled(int field_index)
{
	const struct field_desc *field;

	if (field_index < 0 || field_index >= NUM_FIELDS)
		return 0;

	field = &all_fields[field_index];

	/*
	 * Uncore frequency is part of the frequency group, but it should only
	 * take a default column slot when the platform actually exposes a usable
	 * devfreq-backed uncore source.
	 */
	if (field->id && strcmp(field->id, "uncore_freq") == 0 &&
	    !has_uncore_freq_support())
		return 0;

	if (field->series == FIELD_SERIES_IDLE_STATE &&
	    !*field->enabled_ptr)
		return 0;

	if (field->series == FIELD_SERIES_SUMMARY_TEMP &&
	    !*field->enabled_ptr)
		return 0;

	if (field->id && strcmp(field->id, "cpu_temp_c") == 0 &&
	    get_temp_numa_count() <= 0)
		return 0;

	if (field_hide_mask & (1ULL << field_index))
		return 0;
	if (field_show_mask & (1ULL << field_index))
		return 1;
	if (field_show_mask)
		return 0;  /* whitelist: only explicitly shown fields */
	return *field->enabled_ptr;
}

void clear_field_overrides(void)
{
	field_show_mask = 0;
	field_hide_mask = 0;
}

void set_field_override_by_index(int field_index, int enable, int whitelist)
{
	uint64_t bit;

	(void)whitelist;
	if (field_index < 0 || field_index >= NUM_FIELDS)
		return;

	bit = 1ULL << field_index;
	if (enable) {
		field_show_mask |= bit;
		field_hide_mask &= ~bit;
	} else {
		field_hide_mask |= bit;
		field_show_mask &= ~bit;
	}
}

/*
 * Get field descriptor by ID
 */
struct field_desc *get_field_desc(const char *field_id)
{
	for (int i = 0; i < NUM_FIELDS; i++) {
		if (strcmp(all_fields[i].id, field_id) == 0)
			return &all_fields[i];
	}
	return NULL;
}

/*
 * Get all fields
 */
struct field_desc *get_all_fields(void)
{
	return all_fields;
}

int get_field_count(void)
{
	return NUM_FIELDS;
}

/*
 * Check if any fields of a given scope are enabled
 */
int any_fields_enabled(enum field_scope scope)
{
	for (int i = 0; i < NUM_FIELDS; i++) {
		if (all_fields[i].scope == scope && field_is_effectively_enabled(i))
			return 1;
	}
	return 0;
}

/*
 * Get enabled fields for a scope
 */
void get_enabled_fields(enum field_scope scope, struct field_desc **fields, int *count)
{
	*count = 0;
	for (int i = 0; i < NUM_FIELDS; i++) {
		if (all_fields[i].scope == scope && field_is_effectively_enabled(i)) {
			fields[(*count)++] = &all_fields[i];
		}
	}
}

/* ============================================================================
 * SECTION 5: RECORD BUILD/FREE
 * ============================================================================ */

static struct interval_record *allocate_interval_record(int tracked_count)
{
	struct interval_record *rec;

	if (pool_initialized && rec_pool) {
		rec = rec_pool;
		memset(rec, 0, sizeof(*rec));

		if (tracked_count <= cpu_rows_pool_size) {
			rec->cpu_rows = cpu_rows_pool;
			rec->cpu_rows_is_temp = 0;
			return rec;
		}

		{
			int old_pool_size = cpu_rows_pool_size;
			void *new_pool = realloc(cpu_rows_pool,
						 tracked_count * sizeof(struct cpu_row));
			if (new_pool) {
				cpu_rows_pool = new_pool;
				rec->cpu_rows = cpu_rows_pool;
				rec->cpu_rows_is_temp = 0;
				memset(cpu_rows_pool + old_pool_size, 0,
				       (tracked_count - old_pool_size) *
				       sizeof(struct cpu_row));
				cpu_rows_pool_size = tracked_count;
				return rec;
			}
		}

		rec->cpu_rows = calloc(tracked_count, sizeof(struct cpu_row));
		rec->cpu_rows_is_temp = 1;
		return rec;
	}

	rec = calloc(1, sizeof(*rec));
	if (!rec)
		return NULL;

	if (tracked_count > 0) {
		rec->cpu_rows = calloc(tracked_count, sizeof(struct cpu_row));
		if (!rec->cpu_rows) {
			free(rec);
			return NULL;
		}
		rec->cpu_rows_is_temp = 1;
	}

	return rec;
}

static void fill_record_metadata(struct interval_record *rec,
				 const struct sys_snapshot *raw,
				 int iteration,
				 int tracked_count)
{
	rec->interval = iteration;
	rec->timestamp = time(NULL);
	rec->cpu_count = raw->effective_cpu_count;
	rec->cpu_count_filtered = tracked_count;
	rec->cpu_truncated = raw->cpu_truncated;
	rec->cpu_row_count = tracked_count;
	rec->pmu_event_count = get_pmu_event_count();
}

static void fill_record_summary(struct interval_record *rec,
				const struct sys_snapshot *raw,
				const struct interval_stats *stats)
{
	rec->summary.avg_mhz = stats->avg_mhz;
	rec->summary.uncore_freq_mhz = raw ? (raw->uncore_freq_hz / 1000000.0) : 0.0;
	rec->summary.busy_percent = stats->busy_percent;
	rec->summary.idle_percent = stats->avg_idle_percent;
	rec->summary.iowait_percent = stats->avg_iowait_percent;
	rec->summary.power_mw = stats->avg_power_mw;
	rec->summary.energy_joules = stats->interval_energy_joules;
	rec->summary.mem_bw = stats->mem_bw;
	rec->summary.ctx_switches = stats->ctx_switches;
	rec->summary.interrupts = stats->interrupts;
	rec->summary.soft_interrupts = stats->soft_interrupts;
	rec->summary.ipc = stats->ipc;
	rec->summary.pmu_count = get_pmu_event_count();

	for (int i = 0; i < rec->summary.pmu_count; i++)
		rec->summary.pmu[i] = stats->pmu_delta[i];
}

static void fill_cpu_rows(struct interval_record *rec, int tracked_count)
{
	if (tracked_count <= 0)
		return;

	for (int i = 0; i < tracked_count; i++)
		rec->cpu_rows[i].cpu_idx = i;
}

struct interval_record *build_interval_record(
	const struct sys_snapshot *raw,
	const struct interval_stats *stats,
	int iteration)
{
	struct interval_record *rec;
	int tracked_count = raw->effective_cpu_count;

	rec = allocate_interval_record(tracked_count);
	if (!rec)
		return NULL;

	if (tracked_count > 0 && !rec->cpu_rows) {
		if (rec != rec_pool)
			free(rec);
		return NULL;
	}

	/* Store pointers to source data */
	rec->raw = raw;
	rec->stats = stats;

	fill_record_metadata(rec, raw, iteration, tracked_count);
	fill_record_summary(rec, raw, stats);
	fill_cpu_rows(rec, tracked_count);

	return rec;
}

void free_interval_record(struct interval_record *rec)
{
	if (!rec)
		return;

	if (rec->cpu_rows_is_temp) {
		free(rec->cpu_rows);
		rec->cpu_rows = NULL;
	}
	if (rec != rec_pool) {
		free(rec);
	}
}

/* ============================================================================
 * SECTION 6: POOL MANAGEMENT
 * ============================================================================ */

void setup_formatter_pool(int max_cpus)
{
	if (rec_pool) {
		free(rec_pool);
		rec_pool = NULL;
	}
	if (cpu_rows_pool) {
		free(cpu_rows_pool);
		cpu_rows_pool = NULL;
	}

	rec_pool = calloc(1, sizeof(struct interval_record));
	cpu_rows_pool = calloc(max_cpus, sizeof(struct cpu_row));

	if (!rec_pool || !cpu_rows_pool) {
		fprintf(stderr, "Error: failed to allocate formatter pool\n");
		free(rec_pool);
		free(cpu_rows_pool);
		rec_pool = NULL;
		cpu_rows_pool = NULL;
		cpu_rows_pool_size = 0;
		pool_initialized = 0;
		return;
	}

	cpu_rows_pool_size = max_cpus;
	pool_initialized = 1;
}

void cleanup_formatter_pool(void)
{
	if (rec_pool) {
		free(rec_pool);
		rec_pool = NULL;
	}
	if (cpu_rows_pool) {
		free(cpu_rows_pool);
		cpu_rows_pool = NULL;
	}
	cpu_rows_pool_size = 0;
	pool_initialized = 0;
}

/* ============================================================================
 * SECTION 7: COLUMN ENABLE/DISABLE
 * ============================================================================ */

/*
 * Update idle state visibility based on actual state count.
 * Call this after cpuidle is initialized to show only existing states.
 */
void update_idle_state_visibility(void)
{
	int state_count = get_global_idle_state_count();

	/* If cpuidle is not enabled/available, hide per-state residency columns entirely. */
	if (!is_cpuidle_enabled() || state_count <= 0) {
		clear_idle_state_columns();
		set_default_idle_state_labels();
		return;
	}

	for (int i = 0; i < ARRAY_SIZE(show_idle_state); i++) {
		const char *name = get_idle_state_name(i);
		int available = (i < state_count);
		int enabled;

		if (name && *name) {
			snprintf(idle_state_labels[i], sizeof(idle_state_labels[i]), "%s", name);
		} else {
			snprintf(idle_state_labels[i], sizeof(idle_state_labels[i]), "LPI-%d", i);
		}
		snprintf(idle_state_wakeup_labels[i],
			 sizeof(idle_state_wakeup_labels[i]), "%s_wake",
			 idle_state_labels[i]);

		if (!available) {
			enabled = 0;
		} else if (idle_hide_mask & (1U << i)) {
			enabled = 0;
		} else if (idle_show_mask & (1U << i)) {
			enabled = 1;
		} else if (idle_show_mask) {
			enabled = 0;  /* whitelist: only explicitly shown states */
		} else {
			enabled = 1;
		}

		show_idle_state[i] = enabled;
		show_summary_idle_state[i] = enabled;
	}
}

int idle_state_columns_enabled(void)
{
	for (int i = 0; i < ARRAY_SIZE(show_idle_state); i++) {
		if (show_idle_state[i] || show_summary_idle_state[i])
			return 1;
	}
	return 0;
}

void enable_cpu(int e)    { show_cpu    = e; }
void enable_freq(int e)   { show_freq   = e; }
void enable_idle(int e)
{
	show_idle = e;
	if (e)
		update_idle_state_visibility();
	else
		clear_idle_state_columns();
}
void enable_iowait(int e) { show_iowait = e; }
void enable_power(int e)  { show_power  = e; }
void enable_temp(int e)   { show_temp   = e; }
void enable_pmu(int e)    { show_pmu    = e; }
void enable_sysstat(int e){ show_sysstat = e; }
void enable_membw(int e)  { show_membw  = e; }
void enable_numa(int e)   { show_numa   = e; }
void enable_package(int e){ show_package = e; }
void enable_core(int e)   { show_core   = e; }
void enable_ipc(int e)    { show_ipc    = e; }
void enable_energy(int e) { show_energy = e; }

void update_temp_field_visibility(void)
{
	int count = get_temp_numa_count();

	for (int i = 0; i < ARRAY_SIZE(show_temp_vdie); i++)
		show_temp_vdie[i] = show_temp && (i < count);
}

static void set_all_show_flags(int val)
{
	show_cpu    = val;
	show_freq   = val;
	show_idle   = val;
	show_iowait = 0;
	show_power  = val;
	show_temp   = val;
	show_pmu    = 0;
	show_sysstat= 0;
	show_membw  = 0;
	show_package= 0;
	show_core   = 0;
	show_numa   = 0;
	show_ipc    = 0;
	show_energy = 0;
}

void reset_columns(void)
{
	set_all_show_flags(1);
	update_temp_field_visibility();
	clear_field_overrides();
	reset_idle_state_overrides_internal();
	set_idle_state_columns_visible(2); /* default: first 2 states */
}

void clear_columns(void)
{
	set_all_show_flags(0);
	update_temp_field_visibility();
	clear_field_overrides();
	reset_idle_state_overrides_internal();
	clear_idle_state_columns();
}
