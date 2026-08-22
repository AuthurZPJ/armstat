/* SPDX-License-Identifier: GPL-2.0 */
/*
 * aggregator.h - Aggregation and calculation layer
 *
 * RESPONSIBLE FOR:
 *   - Delta calculations from snapshot data
 *   - Averages and percentages
 *   - Interval statistics (CPU freq, idle, ctx switches, etc.)
 *   - Power and memory bandwidth interval calculations (using unified delta)
 *
 * NOT RESPONSIBLE FOR:
 *   - Any sysfs/proc I/O (done in collector)
 *   - Output formatting
 */

#ifndef ARMSTAT_AGGREGATOR_H
#define ARMSTAT_AGGREGATOR_H

#include "collector.h"

#define MAX_PACKAGES MAX_CPUS

/*
 * Per-package aggregated statistics
 */
struct package_row {
	int package_id;
	int cpu_count;
	double avg_mhz;
	double idle_percent;
	double busy_percent;
	double iowait_percent;
};

/*
 * Aggregated statistics for one interval
 */
struct interval_stats {
	/* System-wide frequency statistics */
	double avg_mhz;
	double busy_percent;

	/* System-wide idle statistics */
	double avg_idle_percent;
	double avg_iowait_percent;

	/* Per-CPU statistics (for CPU rows) */
	double per_cpu_mhz[MAX_CPUS];
	double per_cpu_busy[MAX_CPUS];
	double per_cpu_idle[MAX_CPUS];
	double per_cpu_iowait[MAX_CPUS];

	/* Power/Energy */
	double avg_power_mw;
	double interval_energy_joules;

	/* IPC (Instructions Per Cycle) */
	double ipc;

	/* Memory bandwidth (MiB/s) */
	double mem_bw;

	/* System stats deltas */
	double ctx_switches;
	double interrupts;
	double soft_interrupts;

	/* PMU deltas */
	unsigned long long pmu_delta[MAX_PMU_EVENTS];
	unsigned long long per_cpu_pmu[MAX_CPUS][MAX_PMU_EVENTS];
	unsigned char per_cpu_pmu_valid[MAX_CPUS];
	int pmu_valid;
	double per_cpu_ipc[MAX_CPUS];

	/* Per-package aggregation */
	struct package_row packages[MAX_PACKAGES];
	int package_count;
};

/*
 * Initialize aggregator
 * Returns: 0 on success
 */
int init_aggregator(void);

/*
 * Calculate interval statistics from raw snapshot
 * Must be called after collect_snapshot()
 *
 * @raw: Raw snapshot from collector
 * @stats: Output for calculated interval statistics
 */
void calculate_interval_stats(const struct sys_snapshot *raw, struct interval_stats *stats);

/*
 * Reset aggregator state (call when starting new measurement)
 */
void reset_aggregator(void);

/*
 * Cleanup aggregator
 */
void cleanup_aggregator(void);

#endif /* ARMSTAT_AGGREGATOR_H */
