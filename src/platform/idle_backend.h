/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_IDLE_BACKEND_H
#define ARMSTAT_IDLE_BACKEND_H

/*
 * idle_backend.h - Busy/Idle source policy helpers
 *
 * Busy/Idle percentages are now derived from raw cumulative counters captured
 * in sample_cache.c and turned into interval deltas by aggregator.c.
 *
 * This module no longer owns a runtime "idle backend" object. Its only job is
 * to answer policy questions such as:
 *   - which busy-source mode is selected?
 *   - should a given CPU use /proc/stat or /proc/schedstat?
 *   - which CPUs are listed in /sys/devices/system/cpu/nohz_full?
 */

/*
 * busy_source_mode - authoritative Busy%/Idle% source selection
 *
 * PROCSTAT:
 *   Use /proc/stat idle accounting for all tracked CPUs.
 *
 * SCHEDSTAT:
 *   Use /proc/schedstat per-CPU runtime as the Busy% authority and derive
 *   Idle% from the remaining wall-clock interval.
 *
 * TASK_CLOCK:
 *   Legacy compatibility alias. Current releases resolve this to the same
 *   implementation as SCHEDSTAT because CPU-wide perf task-clock did not
 *   provide a reliable Busy/Idle split on the target ARM servers.
 *
 * AUTO:
 *   Default policy. Use /proc/stat on ordinary CPUs and prefer schedstat on
 *   CPUs listed in /sys/devices/system/cpu/nohz_full when schedstat is
 *   available.
 */
enum busy_source_mode {
	BUSY_SOURCE_AUTO = 0,
	BUSY_SOURCE_PROCSTAT,
	BUSY_SOURCE_SCHEDSTAT,
	BUSY_SOURCE_TASK_CLOCK,
};

void set_busy_source_mode(enum busy_source_mode mode);
enum busy_source_mode get_busy_source_mode(void);
const char *get_busy_source_mode_name(void);
const char *get_busy_source_effective_name(void);

/*
 * Returns non-zero when this CPU should use /proc/schedstat runtime instead of
 * /proc/stat idle counters for authoritative Busy/Idle accounting.
 */
int busy_source_uses_schedstat_cpu(int cpu_id);

int cpu_is_nohz_full(int cpu_id);
int get_nohz_full_cpu_count(void);

#endif /* ARMSTAT_IDLE_BACKEND_H */
