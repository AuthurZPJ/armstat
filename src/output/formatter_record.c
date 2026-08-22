/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_record.c - Build and own one self-contained interval record
 *
 * Materializes raw and aggregated interval data before serialization. This
 * file owns record allocation, reusable pools, and the raw-to-record copy.
 * Typed field getters live separately in formatter_values.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "formatter.h"
#include "idle_display.h"
#include "pmu.h"
#include "topology.h"
#include "cpu_inventory.h"
#include "cpuidle.h"

/* Reusable allocations for the one-record-at-a-time sampling loop. */
static struct interval_record *rec_pool;
static struct cpu_row *cpu_rows_pool;
static int cpu_rows_pool_size;
static int pool_initialized;

static double clamp_percent(double pct)
{
	if (pct < 0.0)
		return 0.0;
	if (pct > 100.0)
		return 100.0;
	return pct;
}

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

/* Record build/free ------------------------------------------------------- */

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
				 unsigned long long iteration,
				 int tracked_count)
{
	struct timespec ts;

	rec->interval = iteration;
	rec->duration_us = sys_snapshot_get_interval_delta_us(raw);
	if (raw && raw->sample_timestamp_ns) {
		rec->timestamp = raw->sample_timestamp;
		rec->timestamp_ns = raw->sample_timestamp_ns;
	} else if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
		rec->timestamp = ts.tv_sec;
		rec->timestamp_ns = (unsigned long long)ts.tv_sec * 1000000000ULL +
				    (unsigned long long)ts.tv_nsec;
	} else {
		rec->timestamp = time(NULL);
		rec->timestamp_ns =
			(unsigned long long)rec->timestamp * 1000000000ULL;
	}
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
	rec->summary.uncore_freq_mhz = raw && raw->uncore_freq_valid ?
		raw->uncore_freq_hz / 1000000.0 : NAN;
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
	rec->summary.pmu_valid = stats->pmu_valid;

	for (int i = 0; i < rec->summary.pmu_count; i++)
		rec->summary.pmu[i] = stats->pmu_delta[i];
}

static void materialize_cpu_idle_usage(struct cpu_row *row,
				       const struct sys_snapshot *raw,
				       int cpu_idx)
{
	for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++) {
		const struct idle_state *state =
			get_usable_raw_idle_state(raw, cpu_idx, s);

		row->idle_state_usage[s] = state ? state->usage_per_sec : NAN;
	}
}

static double compute_cpu_temp_c(const struct sys_snapshot *raw, int cpu_idx)
{
	int cpu_id;
	int numa;

	if (!raw || cpu_idx < 0)
		return NAN;

	cpu_id = get_cpu_id_by_tracked_idx(cpu_idx);
	if (cpu_id < 0)
		return NAN;

	numa = get_numa_node(cpu_id);
	if (numa < 0 || numa >= raw->numa_temp_count ||
	    !(raw->numa_temp_valid_mask & (1U << numa)))
		return NAN;

	return raw->numa_temps[numa] / 1000.0;
}

static void materialize_cpu_rows(struct interval_record *rec,
				 const struct sys_snapshot *raw,
				 const struct interval_stats *stats,
				 int tracked_count)
{
	const struct cpu_freq_info *freqs = sys_snapshot_get_freqs(raw);
	int pmu_count = get_pmu_event_count();

	if (tracked_count <= 0)
		return;

	for (int i = 0; i < tracked_count; i++) {
		struct cpu_row *row = &rec->cpu_rows[i];
		double idle_pct;

		memset(row, 0, sizeof(*row));
		row->cpu_idx = i;

		if (freqs)
			row->freq = freqs[i];

		idle_pct = stats ? clamp_percent(stats->per_cpu_idle[i]) : 0.0;
		row->idle_percent = stats ? stats->per_cpu_idle[i] : 0.0;
		row->iowait_percent = stats ? stats->per_cpu_iowait[i] : 0.0;
		row->busy_percent = 100.0 - row->idle_percent;
		row->ipc = stats ? stats->per_cpu_ipc[i] : NAN;

		row->temp_c = compute_cpu_temp_c(raw, i);

		compute_idle_state_display(row->idle_state_pct,
					   raw ? (const struct idle_state **)raw->idle : NULL,
					   i,
					   raw ? raw->idle_state_count : 0,
					   idle_pct,
					   show_idle_state, 0);
		materialize_cpu_idle_usage(row, raw, i);

		if (stats) {
			row->pmu_valid = stats->per_cpu_pmu_valid[i];
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
	rec->numa_temp_valid_mask = raw ? raw->numa_temp_valid_mask : 0;
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
		double tmp[MAX_VISIBLE_IDLE_STATES];
		double idle_pct = stats ? clamp_percent(stats->per_cpu_idle[i]) : 0.0;

		compute_idle_state_display(tmp,
					   raw ? (const struct idle_state **)raw->idle : NULL,
					   i,
					   raw ? raw->idle_state_count : 0,
					   idle_pct,
					   show_summary_idle_state, 1);
		for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++)
			acc[s] += tmp[s];
	}

	for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++)
		rec->summary_idle_state_pct[s] = acc[s] / tracked_count;
}

struct interval_record *build_interval_record(
	const struct sys_snapshot *raw,
	const struct interval_stats *stats,
	unsigned long long iteration)
{
	struct interval_record *rec;
	int tracked_count;

	if (!raw || !stats)
		return NULL;

	tracked_count = sys_snapshot_get_effective_cpu_count(raw);

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

/* Reusable pool lifecycle ------------------------------------------------ */

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
