/* SPDX-License-Identifier: GPL-2.0 */
/*
 * System statistics: IRQ, context switches, etc.
 *
 * Monitors system-level events:
 *   - Context switches
 *   - Hardware interrupts (IRQ)
 *   - Soft interrupts
 *   - SMI (System Management Interrupt) - mainly x86
 *
 * Data sources:
 *   - /proc/stat (aggregate system stats)
 *   - /proc/interrupts (per-IRQ counts)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sysstat.h"
#include "collector.h"

/*
 * FD caching for hot path files
 * Avoids repeated fopen/fclose on every sampling interval
 */
#define MAX_SYSSTAT_FDS	2
/* MAX_CPUS is defined in collector.h — single source of truth */
static struct {
	const char *path;
	FILE *fp;
	int valid;
} sysstat_fds[MAX_SYSSTAT_FDS] = {
	{ "/proc/stat", NULL, 0 },
	{ "/proc/schedstat", NULL, 0 },
};

static FILE *get_sysstat_fp(const char *path)
{
	int i;
	for (i = 0; i < MAX_SYSSTAT_FDS && sysstat_fds[i].path; i++) {
		if (strcmp(sysstat_fds[i].path, path) == 0) {
			if (sysstat_fds[i].valid && sysstat_fds[i].fp) {
				/* Seek to beginning for re-read */
				rewind(sysstat_fds[i].fp);
				return sysstat_fds[i].fp;
			}
			/* Open and cache */
			sysstat_fds[i].fp = fopen(path, "r");
			if (sysstat_fds[i].fp) {
				sysstat_fds[i].valid = 1;
				return sysstat_fds[i].fp;
			}
			return NULL;
		}
	}
	/* Not in cache, fall back to regular fopen */
	return fopen(path, "r");
}

void close_sysstat_fds(void)
{
	int i;
	for (i = 0; i < MAX_SYSSTAT_FDS && sysstat_fds[i].path; i++) {
		if (sysstat_fds[i].fp) {
			fclose(sysstat_fds[i].fp);
			sysstat_fds[i].fp = NULL;
			sysstat_fds[i].valid = 0;
		}
	}
}

/*
 * Cached parsed values from /proc/stat
 * Avoids parsing the file 3 times per interval
 */
static int proc_stat_valid = 0;
static unsigned long long cached_ctxt = 0;
static unsigned long long cached_intr = 0;
static unsigned long long cached_softirq = 0;
static unsigned long long cached_cpu_idle[MAX_CPUS];
static unsigned long long cached_cpu_iowait[MAX_CPUS];
static int cached_highest_cpu_idx = -1;
static int schedstat_valid = 0;
static unsigned long long cached_schedstat_runtime[MAX_CPUS];
static int cached_schedstat_highest_cpu_idx = -1;

/*
 * invalidate_proc_stat_cache - Invalidate /proc/stat cache for fresh read
 * Must be called at the start of each collection interval
 */
void invalidate_proc_stat_cache(void)
{
	proc_stat_valid = 0;
	schedstat_valid = 0;
}

static void refresh_proc_stat(void)
{
	FILE *fp = get_sysstat_fp("/proc/stat");
	unsigned long long next_ctxt = 0;
	unsigned long long next_intr = 0;
	unsigned long long next_softirq = 0;
	static unsigned long long next_cpu_idle[MAX_CPUS];
	static unsigned long long next_cpu_iowait[MAX_CPUS];
	int next_highest_cpu_idx = -1;
	int saw_data = 0;

	if (!fp)
		return;

	char line[PROC_LINE_MAX];
	memset(next_cpu_idle, 0, sizeof(next_cpu_idle));
	memset(next_cpu_iowait, 0, sizeof(next_cpu_iowait));

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '9') {
			int cpu_idx = atoi(line + 3);
			if (cpu_idx >= 0 && cpu_idx < MAX_CPUS) {
				unsigned long long user, nice, system, idle_val, iowait_val;
				char *data_start = line + 3;

				while (*data_start >= '0' && *data_start <= '9')
					data_start++;
				while (*data_start == ' ')
					data_start++;

				int n = sscanf(data_start,
					       "%llu %llu %llu %llu %llu",
					       &user, &nice, &system, &idle_val, &iowait_val);
				if (n >= 4) {
					next_cpu_idle[cpu_idx] = idle_val;
					next_cpu_iowait[cpu_idx] = (n >= 5) ? iowait_val : 0;
					if (cpu_idx > next_highest_cpu_idx)
						next_highest_cpu_idx = cpu_idx;
					saw_data = 1;
				}
			}
		} else if (strncmp(line, "ctxt ", 5) == 0) {
			sscanf(line + 5, "%llu", &next_ctxt);
			saw_data = 1;
		} else if (strncmp(line, "intr ", 5) == 0) {
			sscanf(line + 5, "%llu", &next_intr);
			saw_data = 1;
		} else if (strncmp(line, "softirq ", 8) == 0) {
			sscanf(line + 8, "%llu", &next_softirq);
			saw_data = 1;
		}
	}

	if (!saw_data)
		return;

	cached_ctxt = next_ctxt;
	cached_intr = next_intr;
	cached_softirq = next_softirq;
	cached_highest_cpu_idx = next_highest_cpu_idx;
	memcpy(cached_cpu_idle, next_cpu_idle, sizeof(cached_cpu_idle));
	memcpy(cached_cpu_iowait, next_cpu_iowait, sizeof(cached_cpu_iowait));
	proc_stat_valid = 1;
}

int init_sysstat(void)
{
	return 0;
}

int read_ctx_switches(unsigned long long *count)
{
	/* Only refresh if not yet cached this interval (single read for all fields) */
	if (!proc_stat_valid)
		refresh_proc_stat();
	*count = cached_ctxt;
	return 0;
}

int read_interrupts(unsigned long long *count)
{
	/* Only refresh if not yet cached this interval (single read for all fields) */
	if (!proc_stat_valid)
		refresh_proc_stat();
	*count = cached_intr;
	return 0;
}

int read_soft_interrupts(unsigned long long *count)
{
	/* Only refresh if not yet cached this interval (single read for all fields) */
	if (!proc_stat_valid)
		refresh_proc_stat();
	*count = cached_softirq;
	return 0;
}

int read_all_proc_stat_cpu_idle(unsigned long long *idles,
				 unsigned long long *iowaits,
				 int max_cpus)
{
	int available;
	int copy_count;

	if (!proc_stat_valid)
		refresh_proc_stat();

	if (cached_highest_cpu_idx < 0)
		return -1;

	available = cached_highest_cpu_idx + 1;
	copy_count = available < max_cpus ? available : max_cpus;

	if (idles)
		memcpy(idles, cached_cpu_idle, copy_count * sizeof(*idles));
	if (iowaits)
		memcpy(iowaits, cached_cpu_iowait, copy_count * sizeof(*iowaits));

	return available;
}

static void refresh_schedstat(void)
{
	FILE *fp = get_sysstat_fp("/proc/schedstat");
	char line[PROC_LINE_MAX];
	static unsigned long long next_runtime[MAX_CPUS];
	int next_highest_cpu_idx = -1;
	int saw_data = 0;

	if (!fp)
		return;

	memset(next_runtime, 0, sizeof(next_runtime));

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '9') {
			int cpu_idx;
			unsigned long long fields[9] = {0};

		if (sscanf(line,
			   "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
			   &cpu_idx,
			   &fields[0], &fields[1], &fields[2], &fields[3],
			   &fields[4], &fields[5], &fields[6], &fields[7]) >= 9) {
			if (cpu_idx >= 0 && cpu_idx < MAX_CPUS) {
				next_runtime[cpu_idx] = fields[5];
					if (cpu_idx > next_highest_cpu_idx)
						next_highest_cpu_idx = cpu_idx;
					saw_data = 1;
				}
			}
		}
	}

	if (!saw_data)
		return;

	memcpy(cached_schedstat_runtime, next_runtime,
	       sizeof(cached_schedstat_runtime));
	cached_schedstat_highest_cpu_idx = next_highest_cpu_idx;
	schedstat_valid = 1;
}

int read_all_schedstat_cpu_runtime(unsigned long long *runtime_ns, int max_cpus)
{
	int available;
	int copy_count;

	if (!schedstat_valid)
		refresh_schedstat();

	if (cached_schedstat_highest_cpu_idx < 0)
		return -1;

	available = cached_schedstat_highest_cpu_idx + 1;
	copy_count = available < max_cpus ? available : max_cpus;

	if (runtime_ns)
		memcpy(runtime_ns, cached_schedstat_runtime,
		       copy_count * sizeof(*runtime_ns));

	return available;
}

/* Cache for HZ value */
static int cached_hz = 0;

/*
 * get_kernel_hz - Get kernel timer HZ (jiffies per second)
 *
 * WHY THIS MATTERS: /proc/stat idle times are in jiffies, not microseconds.
 * To convert to usec for delta calculation, we need the actual HZ value.
 *
 * STRATEGY & LIMITATIONS:
 *   1. sysconf(_SC_CLK_TCK): POSIX standard, most reliable
 *      - Works on all modern Linux systems
 *      - Returns actual CONFIG_HZ value
 *
 *   2. /sys/kernel/time/timer_res_ms: Timer resolution fallback
 *      - Only exists on some systems
 *      - Inverse of HZ (e.g., 10ms -> 100 HZ)
 *
 *   3. /proc/cmdline hz= parsing: Boot param fallback
 *      - Rarely needed
 *      - May not reflect runtime HZ if turbo governor active
 *
 * LIMITATION: On ARM servers, HZ may vary (dynamic tick) or be non-standard.
 * If all methods fail, defaults to 100 - this may cause slight inaccuracy
 * in idle percentage calculations but won't break functionality.
 */
int get_kernel_hz(void)
{
	long hz;

	if (cached_hz > 0)
		return cached_hz;

	/* Method 1: POSIX standard - sysconf(_SC_CLK_TCK) */
	hz = sysconf(_SC_CLK_TCK);
	if (hz > 0) {
		cached_hz = (int)hz;
		return cached_hz;
	}

	/* Method 2: Try /sys/kernel/time/timer_res_ms */
	FILE *fp = fopen("/sys/kernel/time/timer_res_ms", "r");
	if (fp) {
		int res_ms;
		if (fscanf(fp, "%d", &res_ms) == 1 && res_ms > 0) {
			cached_hz = 1000 / res_ms;
			if (cached_hz > 0) {
				fclose(fp);
				return cached_hz;
			}
		}
		fclose(fp);
	}

	/* Method 3: Try to parse /proc/cmdline for hz= */
	fp = fopen("/proc/cmdline", "r");
	if (fp) {
		char line[PROC_LINE_MAX];
		if (fgets(line, sizeof(line), fp)) {
			char *p = strstr(line, "hz=");
			if (!p)
				p = strstr(line, "HZ=");
			if (p) {
				p += 3;
				int val = atoi(p);
				if (val > 0) {
					cached_hz = val;
					fclose(fp);
					return cached_hz;
				}
			}
		}
		fclose(fp);
	}

	/* Last resort: default to 100 (common for ARM servers) */
	/* This should rarely be reached on modern Linux */
	cached_hz = 100;
	return cached_hz;
}
