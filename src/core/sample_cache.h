/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_SAMPLE_CACHE_H
#define ARMSTAT_SAMPLE_CACHE_H

#include "collector.h"

/*
 * Sample Cache - Memory pools and per-interval fast-path data collection
 *
 * Manages pre-allocated buffers for:
 *   - Per-CPU frequency, power, temperature arrays
 *   - Authoritative Busy/Idle raw counters for aggregator delta math
 *   - PMU per-CPU counter snapshots
 */

/*
 * Initialize sample cache (memory pools)
 * @cpu_count: Number of CPUs to allocate for
 */
int init_sample_cache(int cpu_count);

/*
 * Free sample cache
 */
void free_sample_cache(void);

/*
 * Reallocate sample cache pools for new CPU count (after hotplug)
 */
int realloc_sample_cache(int cpu_count);

/*
 * Collect per-interval data into snapshot
 * Called every sampling interval
 * @snapshot: Output structure to fill
 * @delta_us: Time since last collection (microseconds)
 */
void collect_per_interval_data(struct sys_snapshot *snapshot, unsigned long long delta_us);

/*
 * Check if sample cache is initialized
 */
int is_sample_cache_initialized(void);

/* Include schedstat counters in diagnostic snapshots regardless of Busy source. */
void set_diagnostic_counter_collection(int enable);

struct sample_cache_slow_stats {
	unsigned long long work_us;
	unsigned long long time_budget_us;
	unsigned long long last_sweep_us; /* Last completed sweep, excluding startup. */
	unsigned long long max_age_us;
	unsigned int refreshed_cpus;
	unsigned int config_reads;
	unsigned int stale_cpus;
	unsigned int unavailable_cpus;
};

void get_sample_cache_slow_stats(struct sample_cache_slow_stats *stats);

#endif /* ARMSTAT_SAMPLE_CACHE_H */
