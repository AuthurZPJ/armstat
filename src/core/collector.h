/* SPDX-License-Identifier: GPL-2.0 */
/*
 * collector.h - Data collection layer
 *
 * Three-layer sampling architecture for performance:
 *
 * 1. STATIC (init/rescan): CPU count, topology, sensor paths, FD cache
 * 2. SLOW-CHANGING (periodic 5s): min/max freq, governor, sensor availability
 * 3. PER-INTERVAL (every sample): cur_freq, raw counters, power, PMU
 *
 * RESPONSIBLE FOR:
 *   - Reading raw data from sysfs/proc
 *   - Capturing one raw cumulative snapshot per interval
 *   - Tracking the unified wall-clock interval length shared by all metrics
 *
 * NOT RESPONSIBLE FOR:
 *   - Power/memory bandwidth interval calculations (done in aggregator)
 *   - Output formatting
 */

#ifndef ARMSTAT_COLLECTOR_H
#define ARMSTAT_COLLECTOR_H

#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include "cpufreq.h"
#include "cpuidle.h"

#define MAX_PMU_EVENTS    16
#define MAX_CPUS         1024
#define PROC_LINE_MAX     512  /* /proc line buffer; 256 is tight on large machines */

#include <stdio.h>

/* Shared string utility used by cpufreq, sample_cache, and formatter layers. */
static inline void copy_cstring(char *dst, size_t dst_size, const char *src)
{
	if (!dst || dst_size == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dst_size, "%s", src);
}

/*
 * Raw system counters snapshot
 * Contains raw counter values (before delta calculation)
 */
struct raw_counters {
	/* System statistics */
	unsigned long long ctx_switches;
	unsigned long long interrupts;
	unsigned long long soft_interrupts;
	int sysstat_valid;

	/* Memory bandwidth (bytes) */
	unsigned long long mem_bw_counter;
	int mem_bw_valid;

	/* PMU counters */
	uint64_t pmu[MAX_PMU_EVENTS];
	uint64_t (*pmu_per_cpu)[MAX_PMU_EVENTS];
	unsigned char *pmu_per_cpu_valid;
	int pmu_count;
	int pmu_valid;
};

/*
 * System snapshot with all collected data
 * This is the data structure passed from collector to aggregator
 */
struct sys_snapshot {
	/* CPU topology */
	int cpu_count;           /* Actual CPU count from system */
	int effective_cpu_count; /* Capped count (MAX_CPUS) for array bounds */
	int cpu_truncated;       /* Flag: 1 if cpu_count > MAX_CPUS (data truncated) */

	/* ===== RAW PER-CPU DATA ===== */
	struct cpu_freq_info *freqs;           /* cur/min/max freq, governor */
	struct idle_state **idle;              /* per-CPU idle states */

	/* ===== PACKAGE-LEVEL POWER (when a supported sensor is available) ===== */
	long long package_power_mw;            /* Package-level power in mW */
	int package_power_valid;                /* Current interval read succeeded */
	unsigned long long uncore_freq_hz;     /* Uncore/devfreq current frequency in Hz */
	int uncore_freq_valid;

	/* ===== NUMA-LEVEL TEMPERATURES (for vdie0, vdie1) ===== */
	int numa_temps[16];                   /* Temperature per NUMA node (milli-C) */
	int numa_temp_count;                  /* Number of NUMA nodes with temp sensors */
	unsigned int numa_temp_valid_mask;    /* Bit N: numa_temps[N] is current */

	/* ===== RAW SYSTEM COUNTERS ===== */
	struct raw_counters counters;

	/*
	 * Authoritative Busy/Idle raw counters captured once per interval.
	 *
	 * These are stored in tracked-CPU order and converted to interval
	 * percentages by aggregator.c alongside the other delta-based metrics.
	 * Keeping the raw cumulative counters in the snapshot avoids each idle
	 * backend maintaining a separate hidden "previous sample" timeline.
	 */
	unsigned long long *authoritative_idle_jiffies;
	unsigned long long *authoritative_iowait_jiffies;
	unsigned char *authoritative_procstat_valid;
	unsigned long long *authoritative_runtime_ns;
	unsigned char *authoritative_runtime_valid;

	/* ===== RAW COUNTERS FOR AGGREGATOR TO CALCULATE INTERVAL STATS ===== */
	/* Note: Power and memory bandwidth interval stats are calculated in aggregator */

	/* Unified time delta (used by all metrics for consistency) */
	unsigned long long interval_delta_us;  /* Time elapsed since last collection (us) */

	/* Metadata */
	unsigned long long sample_monotonic_ns;
	time_t sample_timestamp;
	unsigned long long sample_timestamp_ns;
	int idle_state_count;
};

/*
 * Initialize collector
 * Returns: 0 on success
 */
int init_collector(void);

/* Rebuild every tracked-CPU-dependent runtime subsystem after hotplug. */
int rebuild_hotplug_dependent_state(void);

/*
 * Collect raw snapshot from sysfs/proc
 *
 * Responsibilities:
 *   - Raw data collection from sysfs/proc
 *   - Capturing raw cumulative counters used by aggregator.c
 *   - Recording the unified wall-clock delta for this interval
 *
 * All interval calculations (Busy/Idle, power, memory bandwidth, PMU deltas,
 * etc.) are done in aggregator.
 *
 * @snapshot: Output structure to fill with raw data
 * Returns: 0 on success, -1 if a trustworthy snapshot cannot be produced.
 */
int collect_snapshot(struct sys_snapshot *snapshot);

/*
 * Cleanup collector
 */
void cleanup_collector(void);

/*
 * Get CPU ID by index in tracked list
 * @idx: index (0-based) into tracked CPU list
 * Returns: CPU ID, or -1 if out of bounds
 */
int get_cpu_id_by_tracked_idx(int tracked_idx);

/*
 * Get tracked CPU count
 */
int get_tracked_cpu_count(void);

/*
 * Snapshot accessors — the seam between the collector and its consumers.
 *
 * Multi-consumer fields are exposed through these getters so that aggregator.c,
 * formatter_record.c, and armstat.c do not dereference sys_snapshot internals
 * directly. Single-consumer fields remain accessed directly for now; a future
 * step can make the struct fully opaque by adding the remaining getters.
 */
int sys_snapshot_get_effective_cpu_count(const struct sys_snapshot *s);
int sys_snapshot_get_cpu_truncated(const struct sys_snapshot *s);
unsigned long long sys_snapshot_get_interval_delta_us(const struct sys_snapshot *s);
const struct cpu_freq_info *sys_snapshot_get_freqs(const struct sys_snapshot *s);
struct raw_counters sys_snapshot_get_counters(const struct sys_snapshot *s);

#endif /* ARMSTAT_COLLECTOR_H */
