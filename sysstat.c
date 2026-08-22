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
#include <ctype.h>
#include <errno.h>
#include <limits.h>

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
	/* Callers use only the fixed cached paths above. */
	return NULL;
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
static unsigned char cached_cpu_valid[MAX_CPUS];
static int cached_highest_cpu_idx = -1;
static int cached_ctxt_valid;
static int cached_intr_valid;
static int cached_softirq_valid;
static int schedstat_valid = 0;
static unsigned long long cached_schedstat_runtime[MAX_CPUS];
static unsigned char cached_schedstat_valid[MAX_CPUS];
static int cached_schedstat_highest_cpu_idx = -1;

#define SCHEDSTAT_MIN_SUPPORTED_VERSION 10
#define SCHEDSTAT_MAX_SUPPORTED_VERSION 17

static int parse_next_ull(const char **cursor, unsigned long long *value)
{
	const char *start;
	char *end;

	if (!cursor || !*cursor || !value)
		return -1;

	start = *cursor;
	while (isspace((unsigned char)*start))
		start++;
	if (*start == '\0' || *start == '-')
		return -1;

	errno = 0;
	*value = strtoull(start, &end, 10);
	if (end == start || errno == ERANGE ||
	    (*end != '\0' && !isspace((unsigned char)*end)))
		return -1;

	*cursor = end;
	return 0;
}

static int cursor_has_value(const char *cursor)
{
	while (cursor && isspace((unsigned char)*cursor))
		cursor++;
	return cursor && *cursor != '\0';
}

int sysstat_parse_cpu_line(const char *line, int *cpu_id,
			   unsigned long long *idle_jiffies,
			   unsigned long long *iowait_jiffies)
{
	unsigned long long user;
	unsigned long long nice;
	unsigned long long system;
	unsigned long long idle;
	unsigned long long iowait = 0;
	const char *cursor;
	char *end = NULL;
	long parsed_cpu;

	if (!line || !cpu_id || !idle_jiffies || !iowait_jiffies)
		return -1;
	if (strncmp(line, "cpu", 3) != 0 ||
	    !isdigit((unsigned char)line[3]))
		return -1;

	errno = 0;
	parsed_cpu = strtol(line + 3, &end, 10);
	if (errno || end == line + 3 || parsed_cpu < 0 ||
	    parsed_cpu > INT_MAX || !end || !isspace((unsigned char)*end))
		return -1;

	cursor = end;
	if (parse_next_ull(&cursor, &user) < 0 ||
	    parse_next_ull(&cursor, &nice) < 0 ||
	    parse_next_ull(&cursor, &system) < 0 ||
	    parse_next_ull(&cursor, &idle) < 0)
		return -1;
	if (cursor_has_value(cursor) &&
	    parse_next_ull(&cursor, &iowait) < 0)
		return -1;

	*cpu_id = (int)parsed_cpu;
	*iowait_jiffies = iowait;
	*idle_jiffies = idle > ULLONG_MAX - iowait ?
		ULLONG_MAX : idle + iowait;
	return 0;
}

int sysstat_parse_schedstat_cpu_line(const char *line, int *cpu_id,
				     unsigned long long *runtime_ns)
{
	unsigned long long fields[9];
	const char *cursor;
	char *end = NULL;
	long parsed_cpu;
	int i;

	if (!line || !cpu_id || !runtime_ns)
		return -1;
	if (strncmp(line, "cpu", 3) != 0 ||
	    !isdigit((unsigned char)line[3]))
		return -1;

	errno = 0;
	parsed_cpu = strtol(line + 3, &end, 10);
	if (errno || end == line + 3 || parsed_cpu < 0 ||
	    parsed_cpu >= MAX_CPUS || !end || !isspace((unsigned char)*end))
		return -1;

	cursor = end;
	for (i = 0; i < 9; i++) {
		if (parse_next_ull(&cursor, &fields[i]) < 0)
			return -1;
	}
	if (cursor_has_value(cursor))
		return -1;

	*cpu_id = (int)parsed_cpu;
	*runtime_ns = fields[6];
	return 0;
}

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
	static unsigned char next_cpu_valid[MAX_CPUS];
	int next_highest_cpu_idx = -1;
	int saw_data = 0;
	int saw_ctxt = 0;
	int saw_intr = 0;
	int saw_softirq = 0;

	if (!fp)
		return;

	char line[PROC_LINE_MAX];
	memset(next_cpu_idle, 0, sizeof(next_cpu_idle));
	memset(next_cpu_iowait, 0, sizeof(next_cpu_iowait));
	memset(next_cpu_valid, 0, sizeof(next_cpu_valid));

	while (fgets(line, sizeof(line), fp)) {
		const char *cursor;

		if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '9') {
			int cpu_idx;
			unsigned long long idle_jiffies;
			unsigned long long iowait_jiffies;

			if (sysstat_parse_cpu_line(line, &cpu_idx, &idle_jiffies,
						    &iowait_jiffies) == 0 &&
			    cpu_idx < MAX_CPUS) {
				next_cpu_idle[cpu_idx] = idle_jiffies;
				next_cpu_iowait[cpu_idx] = iowait_jiffies;
				next_cpu_valid[cpu_idx] = 1;
				if (cpu_idx > next_highest_cpu_idx)
					next_highest_cpu_idx = cpu_idx;
				saw_data = 1;
			}
		} else if (strncmp(line, "ctxt ", 5) == 0) {
			cursor = line + 5;
			if (parse_next_ull(&cursor, &next_ctxt) == 0) {
				saw_data = 1;
				saw_ctxt = 1;
			}
		} else if (strncmp(line, "intr ", 5) == 0) {
			cursor = line + 5;
			if (parse_next_ull(&cursor, &next_intr) == 0) {
				saw_data = 1;
				saw_intr = 1;
			}
		} else if (strncmp(line, "softirq ", 8) == 0) {
			cursor = line + 8;
			if (parse_next_ull(&cursor, &next_softirq) == 0) {
				saw_data = 1;
				saw_softirq = 1;
			}
		}
	}

	if (!saw_data)
		return;

	cached_ctxt = next_ctxt;
	cached_intr = next_intr;
	cached_softirq = next_softirq;
	cached_highest_cpu_idx = next_highest_cpu_idx;
	cached_ctxt_valid = saw_ctxt;
	cached_intr_valid = saw_intr;
	cached_softirq_valid = saw_softirq;
	memcpy(cached_cpu_idle, next_cpu_idle, sizeof(cached_cpu_idle));
	memcpy(cached_cpu_iowait, next_cpu_iowait, sizeof(cached_cpu_iowait));
	memcpy(cached_cpu_valid, next_cpu_valid, sizeof(cached_cpu_valid));
	proc_stat_valid = 1;
}

int read_ctx_switches(unsigned long long *count)
{
	if (!count)
		return -1;

	/* Only refresh if not yet cached this interval (single read for all fields) */
	if (!proc_stat_valid)
		refresh_proc_stat();
	if (!proc_stat_valid || !cached_ctxt_valid)
		return -1;
	*count = cached_ctxt;
	return 0;
}

int read_interrupts(unsigned long long *count)
{
	if (!count)
		return -1;

	/* Only refresh if not yet cached this interval (single read for all fields) */
	if (!proc_stat_valid)
		refresh_proc_stat();
	if (!proc_stat_valid || !cached_intr_valid)
		return -1;
	*count = cached_intr;
	return 0;
}

int read_soft_interrupts(unsigned long long *count)
{
	if (!count)
		return -1;

	/* Only refresh if not yet cached this interval (single read for all fields) */
	if (!proc_stat_valid)
		refresh_proc_stat();
	if (!proc_stat_valid || !cached_softirq_valid)
		return -1;
	*count = cached_softirq;
	return 0;
}

int read_all_proc_stat_cpu_idle_checked(unsigned long long *idles,
					 unsigned long long *iowaits,
					 unsigned char *valid,
					 int max_cpus)
{
	int available;
	int copy_count;

	if (max_cpus <= 0 || max_cpus > MAX_CPUS)
		return -1;
	if (valid)
		memset(valid, 0, max_cpus * sizeof(*valid));

	if (!proc_stat_valid)
		refresh_proc_stat();

	if (!proc_stat_valid || cached_highest_cpu_idx < 0)
		return -1;

	available = cached_highest_cpu_idx + 1;
	copy_count = available < max_cpus ? available : max_cpus;

	if (idles)
		memcpy(idles, cached_cpu_idle, copy_count * sizeof(*idles));
	if (iowaits)
		memcpy(iowaits, cached_cpu_iowait, copy_count * sizeof(*iowaits));
	if (valid)
		memcpy(valid, cached_cpu_valid, copy_count * sizeof(*valid));

	return available;
}

static void refresh_schedstat(void)
{
	FILE *fp = get_sysstat_fp("/proc/schedstat");
	char line[PROC_LINE_MAX];
	static unsigned long long next_runtime[MAX_CPUS];
	static unsigned char next_valid[MAX_CPUS];
	int next_highest_cpu_idx = -1;
	int saw_data = 0;
	int version = -1;

	if (!fp)
		return;

	memset(next_runtime, 0, sizeof(next_runtime));
	memset(next_valid, 0, sizeof(next_valid));

	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "version ", 8) == 0) {
			const char *cursor = line + 8;
			unsigned long long parsed_version;

			if (parse_next_ull(&cursor, &parsed_version) < 0 ||
			    cursor_has_value(cursor) || parsed_version > INT_MAX)
				version = -1;
			else
				version = (int)parsed_version;
		} else if (strncmp(line, "cpu", 3) == 0 &&
			   line[3] >= '0' && line[3] <= '9') {
			int cpu_idx;
			unsigned long long runtime_ns;

			if (sysstat_parse_schedstat_cpu_line(line, &cpu_idx,
							 &runtime_ns) == 0) {
				next_runtime[cpu_idx] = runtime_ns;
				next_valid[cpu_idx] = 1;
				if (cpu_idx > next_highest_cpu_idx)
					next_highest_cpu_idx = cpu_idx;
				saw_data = 1;
			}
		}
	}

	if (!saw_data || version < SCHEDSTAT_MIN_SUPPORTED_VERSION ||
	    version > SCHEDSTAT_MAX_SUPPORTED_VERSION)
		return;

	memcpy(cached_schedstat_runtime, next_runtime,
	       sizeof(cached_schedstat_runtime));
	memcpy(cached_schedstat_valid, next_valid,
	       sizeof(cached_schedstat_valid));
	cached_schedstat_highest_cpu_idx = next_highest_cpu_idx;
	schedstat_valid = 1;
}

int read_all_schedstat_cpu_runtime_checked(unsigned long long *runtime_ns,
					    unsigned char *valid,
					    int max_cpus)
{
	int available;
	int copy_count;

	if (max_cpus <= 0 || max_cpus > MAX_CPUS)
		return -1;
	if (valid)
		memset(valid, 0, max_cpus * sizeof(*valid));

	if (!schedstat_valid)
		refresh_schedstat();

	if (!schedstat_valid || cached_schedstat_highest_cpu_idx < 0)
		return -1;

	available = cached_schedstat_highest_cpu_idx + 1;
	copy_count = available < max_cpus ? available : max_cpus;

	if (runtime_ns)
		memcpy(runtime_ns, cached_schedstat_runtime,
		       copy_count * sizeof(*runtime_ns));
	if (valid)
		memcpy(valid, cached_schedstat_valid,
		       copy_count * sizeof(*valid));

	return available;
}

/* Cache for HZ value */
static int cached_hz = 0;

/*
 * get_kernel_hz - Get Linux USER_HZ (clock ticks per second)
 *
 * WHY THIS MATTERS: /proc/stat idle times are in jiffies, not microseconds.
 * To convert to usec for delta calculation, use the userspace clock-tick unit
 * reported by sysconf(_SC_CLK_TCK). This is deliberately not CONFIG_HZ or the
 * timer resolution. Linux exposes /proc/stat CPU times in USER_HZ units.
 */
int get_kernel_hz(void)
{
	long hz;

	if (cached_hz > 0)
		return cached_hz;

	hz = sysconf(_SC_CLK_TCK);
	if (hz > 0 && hz <= INT_MAX) {
		cached_hz = (int)hz;
		return cached_hz;
	}

	/* Linux USER_HZ is 100 when sysconf is unexpectedly unavailable. */
	cached_hz = 100;
	return cached_hz;
}
