/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_SYSSTAT_H
#define ARMSTAT_SYSSTAT_H

#include <stdint.h>

/* Raw counter snapshot (for delta calculation) */
struct sys_stat_raw {
	unsigned long long ctx_switches;
	unsigned long long interrupts;
	unsigned long long soft_interrupts;
};

/* Initialize system statistics */
int init_sysstat(void);

/* Invalidate /proc/stat cache for fresh read (call at start of each interval) */
void invalidate_proc_stat_cache(void);

/* Read context switch count */
int read_ctx_switches(unsigned long long *count);

/* Read total interrupt count */
int read_interrupts(unsigned long long *count);

/* Read total soft interrupt count */
int read_soft_interrupts(unsigned long long *count);

/*
 * read_all_proc_stat_cpu_idle - Read all CPU idle times from /proc/stat
 * @idles: Output array for idle times (must have size max_cpus)
 * @iowaits: Output array for iowait times (must have size max_cpus)
 * @max_cpus: Size of output arrays
 *
 * Returns: Highest CPU ID seen + 1, or -1 on error
 *
 * This lets callers safely index sparse CPU IDs (for example CPU100 online
 * with CPUs 0-99 offline) without assuming CPU IDs are dense.
 */
int read_all_proc_stat_cpu_idle(unsigned long long *idles,
			   unsigned long long *iowaits,
			   int max_cpus);

/*
 * read_all_schedstat_cpu_runtime - Read per-CPU runtime from /proc/schedstat
 * @runtime_ns: Output array for runtime in nanoseconds
 * @max_cpus: Size of output array
 *
 * Returns: Highest CPU ID seen + 1, or -1 on error
 *
 * The runtime field represents cumulative task runtime on each CPU in
 * nanoseconds and is useful as a tickless-friendly busy-time source.
 */
int read_all_schedstat_cpu_runtime(unsigned long long *runtime_ns, int max_cpus);

/*
 * get_kernel_hz - Get kernel HZ (clock ticks per second)
 * Uses sysconf(_SC_CLK_TCK) and sysfs/cmdline fallbacks, then defaults to 100
 */
int get_kernel_hz(void);

/* Close cached file descriptors */
void close_sysstat_fds(void);

#endif /* ARMSTAT_SYSSTAT_H */
