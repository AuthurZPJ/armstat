/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter.h - Output formatting layer
 *
 * RESPONSIBLE FOR:
 *   - Text/JSON/CSV output formatting
 *   - Header printing
 *   - Row formatting
 *
 * NOT RESPONSIBLE FOR:
 *   - Reading sysfs/proc
 *   - Calculating statistics
 */

#ifndef ARMSTAT_FORMATTER_H
#define ARMSTAT_FORMATTER_H

#include <time.h>

#include "columns.h"
#include "collector.h"
#include "aggregator.h"
#include "cpufreq.h"

/* Output format types */
#define FORMAT_TEXT  0
#define FORMAT_JSON  1
#define FORMAT_CSV   2

/*
 * CPU row data - per-CPU intermediate model
 *
 * interval_record owns every per-interval dynamic value here; serializers read
 * them directly instead of chasing raw/stats pointers. Static identity fields
 * (package/core/NUMA node) are still looked up lazily at output time from the
 * topology caches via the tracked CPU id.
 */
struct cpu_row {
	int cpu_idx;  /* tracked index; cpu_rows[i].cpu_idx == i */

	/* Owned per-interval frequency snapshot (cur/min/max, governor, boost) */
	struct cpu_freq_info freq;

	double idle_percent;
	double iowait_percent;
	double busy_percent;
	double ipc;                /* NAN when unavailable */
	double temp_c;             /* NAN when unavailable */

	/* Display-adjusted per-idle-state residency (NAN when column hidden) */
	double idle_state_pct[MAX_VISIBLE_IDLE_STATES];
	double idle_state_wakeups[MAX_VISIBLE_IDLE_STATES];

	/* Owned per-CPU PMU counters */
	unsigned long long pmu[MAX_PMU_EVENTS];
	int pmu_valid;
};

/*
 * Summary data - system-wide aggregated statistics
 */
struct summary_data {
	/* Frequency */
	double avg_mhz;
	double uncore_freq_mhz;
	double busy_percent;
	double idle_percent;
	double iowait_percent;

	/* Power/Energy */
	double power_mw;
	double energy_joules;

	/* Memory bandwidth */
	double mem_bw;

	/* Sysstat */
	double ctx_switches;
	double interrupts;
	double soft_interrupts;

	/* PMU events (aggregated in summary mode) */
	unsigned long long pmu[MAX_PMU_EVENTS];
	int pmu_count;
	int pmu_valid;

	/* IPC */
	double ipc;
};

/*
 * Interval record - unified intermediate model
 * Built from sys_snapshot + interval_stats, consumed by serializers
 *
 * The record owns all per-interval values (CPU rows, package rows, summary
 * idle-state residency, NUMA temps) so serializers never dereference the raw
 * snapshot or interval stats after build. Static identity fields (package,
 * core, numa_node) are looked up at output time via the topology caches.
 */
struct interval_record {
	int interval;
	time_t timestamp;
	unsigned long long timestamp_ns;
	unsigned long long duration_us; /* Measured monotonic interval duration */

	/* CPU info */
	int cpu_count;           /* Tracked CPU count */
	int cpu_count_filtered;  /* Tracked count after --cpu sampling filter */
	int cpu_truncated;       /* Truncation warning flag */

	/* PMU event count - global, used for column alignment in text/CSV */
	int pmu_event_count;

	/* Summary (system-wide) data */
	struct summary_data summary;

	/* Owned summary-scope values not part of summary_data */
	double summary_idle_state_pct[MAX_VISIBLE_IDLE_STATES];
	int numa_temps[16];           /* milli-C per NUMA/vdie */
	int numa_temp_count;
	unsigned int numa_temp_valid_mask;

	/* Owned per-package aggregation rows */
	int package_count;
	struct package_row packages[MAX_PACKAGES];

	/* Per-CPU rows (tracked sampling set, dynamic allocation) */
	struct cpu_row *cpu_rows;
	int cpu_row_count;

	/* Flag: cpu_rows is a temp allocation (not from pool) */
	int cpu_rows_is_temp;
};

/*
 * Build interval_record from raw data
 */
struct interval_record *build_interval_record(
	const struct sys_snapshot *raw,
	const struct interval_stats *stats,
	int iteration);

/*
 * Free interval_record
 */
void free_interval_record(struct interval_record *rec);

/*
 * Get the real Linux CPU ID for a row in the record.
 * Hides the tracked_idx → cpu_id translation from the serializers.
 * Returns -1 if rec is NULL or row_idx is out of bounds.
 */
int get_cpu_row_id(const struct interval_record *rec, int row_idx);

/*
 * Text serializer
 */
void serialize_text(const struct interval_record *rec, int iteration);

/*
 * JSON serializer (stateless)
 */
void serialize_json(const struct interval_record *rec, int iteration);

/*
 * CSV serializer
 */
void serialize_csv(const struct interval_record *rec);

/*
 * Setup memory pool for interval_record
 * Called by main to pre-allocate based on expected CPU count
 */
void setup_formatter_pool(int max_cpus);

/*
 * Text serializer configuration
 */
void set_text_quiet(int quiet);
void set_text_header_interval(int interval);

/* CPU list filtering now lives in cpu_inventory; see cpu_inventory.h */

/*
 * Cleanup formatter pool
 */
void cleanup_formatter_pool(void);

/*
 * Close the JSON output array (must be called once when JSON mode ends)
 */
void close_machine_json(void);

/* Reset JSON/CSV stream state before starting an independent output stream. */
void reset_machine_state(void);

#endif /* ARMSTAT_FORMATTER_H */
