/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_record.c - Stage 1: Build interval_record from raw data
 *
 * Builds the unified intermediate model (interval_record) from:
 *   - sys_snapshot (raw hardware counters)
 *   - interval_stats (aggregated statistics)
 *
 * This file owns the value getters (referenced by the all_fields[] table in
 * columns.c) and the record build/free/pool lifecycle. Column visibility and
 * the field descriptor table live in columns.c; see columns.h for that
 * interface.
 *
 * Internal structure map (sections are delimited by === markers):
 *
 *   S2  Helper functions           - clamp/lookup utilities used by S3
 *   S3  Field getter functions     - per-field value extractors (non-static;
 *                                    declared in formatter_fields.h)
 *   S6  Record build/free          - build_interval_record / fill_*
 *   S7  Pool management            - rec_pool / cpu_rows_pool lifecycle
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>

#include "formatter.h"
#include "formatter_fields.h"
#include "aggregator.h"
#include "pmu.h"
#include "topology.h"
#include "cpu_inventory.h"
#include "cpuidle.h"
#include "collector.h"
#include "power.h"

#define ARRAY_SIZE(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

/* Memory pool for interval_record */
static struct interval_record *rec_pool;
static struct cpu_row *cpu_rows_pool;
static int cpu_rows_pool_size;
static int pool_initialized;

/* ============================================================================
 * SECTION 2: HELPER FUNCTIONS
 * ============================================================================ */

static int get_tracked_cpu_id(int tracked_idx)
{
	return get_cpu_id_by_tracked_idx(tracked_idx);
}

static double clamp_percent(double pct)
{
	if (pct < 0.0)
		return 0.0;
	if (pct > 100.0)
		return 100.0;
	return pct;
}

static const struct cpu_row *get_cpu_row(const struct interval_record *rec,
					 int row_idx)
{
	if (!rec || row_idx < 0 || row_idx >= rec->cpu_row_count)
		return NULL;

	return &rec->cpu_rows[row_idx];
}

/*
 * Raw cpuidle state lookup. Only used while materializing owned values during
 * build_interval_record(); serializers read the materialized cpu_row instead.
 */
static const struct idle_state *get_raw_idle_state(const struct sys_snapshot *raw,
						   int cpu_idx, int state_idx)
{
	if (!raw || !raw->idle || cpu_idx < 0)
		return NULL;
	if (state_idx < 0 || state_idx >= raw->idle_state_count)
		return NULL;
	if (!raw->idle[cpu_idx])
		return NULL;

	return &raw->idle[cpu_idx][state_idx];
}

static const struct idle_state *get_usable_raw_idle_state(
	const struct sys_snapshot *raw,
	int cpu_idx, int state_idx)
{
	const struct idle_state *state =
		get_raw_idle_state(raw, cpu_idx, state_idx);

	if (!state || !state->available || state->disabled)
		return NULL;

	return state;
}

/* ============================================================================
 * SECTION 3: FIELD GETTER FUNCTIONS
 * ============================================================================ */

/*
 * Get CPU package ID
 */
int get_cpu_package(const struct interval_record *rec, int row_idx)
{
	(void)rec;
	int cpu_id = get_tracked_cpu_id(row_idx);
	return get_package_id(cpu_id);
}

/*
 * Get CPU core ID
 */
int get_cpu_core(const struct interval_record *rec, int row_idx)
{
	(void)rec;
	int cpu_id = get_tracked_cpu_id(row_idx);
	return get_core_id(cpu_id);
}

/*
 * Get CPU NUMA node
 */
int get_cpu_numa_node(const struct interval_record *rec, int row_idx)
{
	(void)rec;
	int cpu_id = get_tracked_cpu_id(row_idx);
	return get_numa_node(cpu_id);
}

/* --- Frequency getters --- */

double get_cpu_freq_mhz(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->freq.cur_freq / 1000.0;
}

double get_cpu_min_freq_mhz(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->freq.min_freq / 1000.0;
}

double get_cpu_max_freq_mhz(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->freq.max_freq / 1000.0;
}

const char *get_cpu_governor(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return "";
	return row->freq.governor;
}

const char *get_cpu_boost(const struct interval_record *rec, int row_idx)
{
	static const char *const unavailable = "-";
	static const char *const disabled = "0";
	static const char *const enabled = "1";
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return unavailable;
	if (row->freq.boost < 0)
		return unavailable;

	return row->freq.boost ? enabled : disabled;
}

/* --- Idle/busy getters --- */

double get_cpu_busy_percent(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->busy_percent;
}

double get_cpu_idle_percent(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->idle_percent;
}

double get_cpu_iowait_percent(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->iowait_percent;
}

double get_cpu_ipc(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return NAN;
	if (!pmu_is_active())
		return NAN;
	return row->ipc;
}

/* --- Per-idle-state getters --- */

#define DEFINE_CPU_IDLE_STATE_GETTER(state_idx)				\
double get_cpu_idle_state##state_idx(const struct interval_record *rec,	\
				    int row_idx)			\
{									\
	const struct cpu_row *row = get_cpu_row(rec, row_idx);		\
									\
	if (!row)							\
		return NAN;						\
	return row->idle_state_pct[state_idx];				\
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
double get_cpu_idle_state_wakeup##state_idx(const struct interval_record *rec,	\
				    int row_idx)			\
{									\
	const struct cpu_row *row = get_cpu_row(rec, row_idx);		\
									\
	if (!row)							\
		return 0;						\
	return row->idle_state_wakeups[state_idx];			\
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
double get_cpu_temp_c(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->temp_c;
}

/* NUMA temperature getters for SUM level */
static double get_temp_vdie_by_numa(const struct interval_record *rec, int numa)
{
	if (!rec || numa < 0 || numa >= rec->numa_temp_count)
		return 0;
	return rec->numa_temps[numa] / 1000.0;
}

#define DEFINE_NUMA_TEMP_GETTER(numa_idx)				\
double get_temp_vdie##numa_idx(const struct interval_record *rec, int row_idx)	\
{									\
	(void)row_idx;							\
	return get_temp_vdie_by_numa(rec, numa_idx);			\
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
	if (!rec || pkg_idx < 0 || pkg_idx >= rec->package_count)
		return NULL;

	return &rec->packages[pkg_idx];
}

int get_pkg_package_id(const struct interval_record *rec, int row_idx)
{
	/* cpu parameter is actually package index for package-scope fields */
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->package_id : 0;
}

double get_pkg_avg_mhz(const struct interval_record *rec, int row_idx)
{
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->avg_mhz : 0;
}

double get_pkg_idle_percent(const struct interval_record *rec, int row_idx)
{
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->idle_percent : 0;
}

double get_pkg_busy_percent(const struct interval_record *rec, int row_idx)
{
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->busy_percent : 0;
}

double get_pkg_iowait_percent(const struct interval_record *rec, int row_idx)
{
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->iowait_percent : 0;
}

int get_pkg_cpu_count(const struct interval_record *rec, int row_idx)
{
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->cpu_count : 0;
}

/* --- Summary getters --- */

double get_summary_avg_mhz(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.avg_mhz : 0;
}

double get_summary_uncore_freq_mhz(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.uncore_freq_mhz : 0;
}

double get_summary_busy_percent(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.busy_percent : 0;
}

double get_summary_idle_percent(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.idle_percent : 0;
}

double get_summary_iowait_percent(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.iowait_percent : 0;
}

#define DEFINE_SUMMARY_IDLE_STATE_GETTER(state_idx)				\
double get_summary_idle_state##state_idx(const struct interval_record *rec,	\
					 int row_idx)			\
{									\
	(void)row_idx;							\
	if (!rec)							\
		return 0;						\
	return rec->summary_idle_state_pct[state_idx];			\
}

DEFINE_SUMMARY_IDLE_STATE_GETTER(0)
DEFINE_SUMMARY_IDLE_STATE_GETTER(1)
DEFINE_SUMMARY_IDLE_STATE_GETTER(2)
DEFINE_SUMMARY_IDLE_STATE_GETTER(3)
DEFINE_SUMMARY_IDLE_STATE_GETTER(4)
DEFINE_SUMMARY_IDLE_STATE_GETTER(5)
DEFINE_SUMMARY_IDLE_STATE_GETTER(6)
DEFINE_SUMMARY_IDLE_STATE_GETTER(7)

long long get_summary_power_mw(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.power_mw : 0;
}

double get_summary_energy_joules(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.energy_joules : 0;
}

long long get_summary_mem_bw(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? (long long)rec->summary.mem_bw : 0;
}

long long get_summary_ctx_switches(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? (long long)rec->summary.ctx_switches : 0;
}

long long get_summary_interrupts(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? (long long)rec->summary.interrupts : 0;
}

long long get_summary_soft_interrupts(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? (long long)rec->summary.soft_interrupts : 0;
}

double get_summary_ipc(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	if (!rec)
		return NAN;
	if (!pmu_is_active())
		return NAN;
	return rec->summary.ipc;
}

/* ============================================================================
 * SECTION 4: RECORD BUILD/FREE
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
	rec->cpu_count = sys_snapshot_get_effective_cpu_count(raw);
	rec->cpu_count_filtered = tracked_count;
	rec->cpu_truncated = sys_snapshot_get_cpu_truncated(raw);
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

/*
 * LPI display rule (materialized once per interval, then owned by the record):
 *   - Busy/Idle comes from /proc/stat and is treated as authoritative.
 *   - Shallow states keep their raw cpuidle residency percentages.
 *   - The deepest visible usable state becomes the residual bucket so that
 *     sum(LPI-*) is forced to match Idle% for each CPU row.
 *
 * This keeps shallow state visibility intuitive on ARM while avoiding
 * short-interval drift where raw cpuidle state totals undercount idle.
 */
static void compute_cpu_idle_state_display(struct cpu_row *row,
					   const struct sys_snapshot *raw,
					   int cpu_idx, double idle_pct,
					   const int *visible, int summary_mode)
{
	double remaining = idle_pct;
	int state_count = (raw && raw->idle) ? raw->idle_state_count : 0;
	int last_state = -1;
	double hidden_value = summary_mode ? 0.0 : NAN;

	if (state_count > MAX_VISIBLE_IDLE_STATES)
		state_count = MAX_VISIBLE_IDLE_STATES;

	for (int s = state_count - 1; s >= 0; s--) {
		const struct idle_state *state =
			get_usable_raw_idle_state(raw, cpu_idx, s);

		if (state && visible[s]) {
			last_state = s;
			break;
		}
	}

	for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++) {
		const struct idle_state *state =
			get_usable_raw_idle_state(raw, cpu_idx, s);
		double displayed;

		if (s >= state_count || !state || !visible[s]) {
			row->idle_state_pct[s] = hidden_value;
			continue;
		}

		if (s == last_state) {
			displayed = remaining;
		} else {
			displayed = clamp_percent(state->percentage);
			if (displayed > remaining)
				displayed = remaining;
			remaining -= displayed;
			if (remaining < 0.0)
				remaining = 0.0;
		}
		row->idle_state_pct[s] = clamp_percent(displayed);
	}
}

static void materialize_cpu_idle_wakeups(struct cpu_row *row,
					 const struct sys_snapshot *raw,
					 int cpu_idx)
{
	for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++) {
		const struct idle_state *state =
			get_usable_raw_idle_state(raw, cpu_idx, s);

		row->idle_state_wakeups[s] = state ? state->wakeups_per_sec : 0.0;
	}
}

static double compute_cpu_temp_c(const struct sys_snapshot *raw, int cpu_idx)
{
	int cpu_id;
	int numa;

	if (!raw || cpu_idx < 0)
		return 0;

	cpu_id = get_tracked_cpu_id(cpu_idx);
	if (cpu_id < 0)
		return 0;

	numa = get_numa_node(cpu_id);
	if (numa < 0 || numa >= raw->numa_temp_count)
		return 0;

	return raw->numa_temps[numa] / 1000.0;
}

static void materialize_cpu_rows(struct interval_record *rec,
				 const struct sys_snapshot *raw,
				 const struct interval_stats *stats,
				 int tracked_count)
{
	int pmu_count = get_pmu_event_count();

	if (tracked_count <= 0)
		return;

	for (int i = 0; i < tracked_count; i++) {
		struct cpu_row *row = &rec->cpu_rows[i];
		double idle_pct;

		memset(row, 0, sizeof(*row));
		row->cpu_idx = i;

		const struct cpu_freq_info *freqs = sys_snapshot_get_freqs(raw);
		if (freqs)
			row->freq = freqs[i];

		idle_pct = stats ? clamp_percent(stats->per_cpu_idle[i]) : 0.0;
		row->idle_percent = stats ? stats->per_cpu_idle[i] : 0.0;
		row->iowait_percent = stats ? stats->per_cpu_iowait[i] : 0.0;
		row->busy_percent = 100.0 - row->idle_percent;
		row->ipc = stats ? stats->per_cpu_ipc[i] : NAN;

		row->temp_c = compute_cpu_temp_c(raw, i);

		compute_cpu_idle_state_display(row, raw, i, idle_pct,
					      show_idle_state, 0);
		materialize_cpu_idle_wakeups(row, raw, i);

		if (stats) {
			for (int ev = 0; ev < pmu_count && ev < MAX_PMU_EVENTS; ev++)
				row->pmu[ev] = stats->per_cpu_pmu[i][ev];
		}
	}
}

static void materialize_packages(struct interval_record *rec,
				 const struct interval_stats *stats)
{
	rec->package_count = stats ? stats->package_count : 0;
	if (rec->package_count > MAX_PACKAGES)
		rec->package_count = MAX_PACKAGES;

	for (int i = 0; i < rec->package_count; i++)
		rec->packages[i] = stats->packages[i];
}

static void materialize_numa_temps(struct interval_record *rec,
				   const struct sys_snapshot *raw)
{
	rec->numa_temp_count = raw ? raw->numa_temp_count : 0;
	if (rec->numa_temp_count > 16)
		rec->numa_temp_count = 16;

	for (int i = 0; i < rec->numa_temp_count; i++)
		rec->numa_temps[i] = raw->numa_temps[i];
}

static void materialize_summary_idle_states(struct interval_record *rec,
					    const struct sys_snapshot *raw,
					    const struct interval_stats *stats,
					    int tracked_count)
{
	double acc[MAX_VISIBLE_IDLE_STATES] = {0};

	if (tracked_count <= 0) {
		for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++)
			rec->summary_idle_state_pct[s] = 0.0;
		return;
	}

	for (int i = 0; i < tracked_count; i++) {
		struct cpu_row row;
		double idle_pct = stats ? clamp_percent(stats->per_cpu_idle[i]) : 0.0;

		memset(&row, 0, sizeof(row));
		compute_cpu_idle_state_display(&row, raw, i, idle_pct,
					      show_summary_idle_state, 1);
		for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++)
			acc[s] += row.idle_state_pct[s];
	}

	for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++)
		rec->summary_idle_state_pct[s] = acc[s] / tracked_count;
}

struct interval_record *build_interval_record(
	const struct sys_snapshot *raw,
	const struct interval_stats *stats,
	int iteration)
{
	struct interval_record *rec;
	int tracked_count = sys_snapshot_get_effective_cpu_count(raw);

	rec = allocate_interval_record(tracked_count);
	if (!rec)
		return NULL;

	if (tracked_count > 0 && !rec->cpu_rows) {
		if (rec != rec_pool)
			free(rec);
		return NULL;
	}

	fill_record_metadata(rec, raw, iteration, tracked_count);
	fill_record_summary(rec, raw, stats);
	materialize_cpu_rows(rec, raw, stats, tracked_count);
	materialize_packages(rec, stats);
	materialize_numa_temps(rec, raw);
	materialize_summary_idle_states(rec, raw, stats, tracked_count);

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
 * SECTION 5: POOL MANAGEMENT
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
