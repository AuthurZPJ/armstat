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

/* Invalidate /proc/stat cache for fresh read (call at start of each interval) */
void invalidate_proc_stat_cache(void);

/* Read context switch count */
int read_ctx_switches(unsigned long long *count);

/* Read total interrupt count */
int read_interrupts(unsigned long long *count);

/* Read total soft interrupt count */
int read_soft_interrupts(unsigned long long *count);

/*
 * Read all CPU idle times from /proc/stat, also reporting which sparse CPU
 * slots were actually present. Returns highest CPU ID seen + 1, or -1.
 */
int read_all_proc_stat_cpu_idle_checked(unsigned long long *idles,
					 unsigned long long *iowaits,
					 unsigned char *valid,
					 int max_cpus);

/*
 * Parse one per-CPU /proc/stat line.
 *
 * The returned idle value includes iowait, matching armstat's contract that
 * IOWait% is a subset of Idle% rather than additional busy time.
 */
int sysstat_parse_cpu_line(const char *line, int *cpu_id,
			   unsigned long long *idle_jiffies,
			   unsigned long long *iowait_jiffies);

/* Parse the stable nine-field per-CPU schedstat record (runtime is field 7). */
int sysstat_parse_schedstat_cpu_line(const char *line, int *cpu_id,
				     unsigned long long *runtime_ns);

/*
 * Read per-CPU runtime from /proc/schedstat, also reporting which sparse CPU
 * slots were present. Returns highest CPU ID seen + 1, or -1.
 */
int read_all_schedstat_cpu_runtime_checked(unsigned long long *runtime_ns,
					    unsigned char *valid,
					    int max_cpus);

/*
 * get_kernel_hz - Get kernel HZ (clock ticks per second)
 * Uses sysconf(_SC_CLK_TCK), falling back to Linux USER_HZ=100.
 */
int get_kernel_hz(void);

/* Close cached file descriptors */
void close_sysstat_fds(void);

#endif /* ARMSTAT_SYSSTAT_H */
