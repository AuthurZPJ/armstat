/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM PMU event monitoring via perf_event_open
 *
 * The implementation intentionally follows the turbostat model more closely:
 *   - counters are opened per tracked CPU, not hard-coded to CPU0
 *   - raw readings are collected per CPU and aggregated later by aggregator.c
 *   - summary values are derived from the tracked CPU set instead of being
 *     treated as "CPU0 pretending to be system-wide"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <errno.h>
#include <limits.h>

#include "pmu.h"
#include "cpu_inventory.h"

/*
 * Minimal perf_event UAPI subset needed by armstat.
 *
 * We intentionally keep a local, layout-compatible prefix of
 * struct perf_event_attr here instead of including the kernel tree's
 * <linux/perf_event.h>. The tool is built inside the Linux source tree, and
 * pulling the in-tree UAPI header from userland can drag in arch-specific
 * asm headers that do not exist on the build host. The first 64 bytes below
 * match PERF_ATTR_SIZE_VER0, which is sufficient for type/config/read_format
 * and the flags armstat needs.
 */

#define PERF_TYPE_HARDWARE	0
#define PERF_TYPE_RAW		4

#define PERF_COUNT_HW_CPU_CYCLES		0
#define PERF_COUNT_HW_INSTRUCTIONS		1
#define PERF_COUNT_HW_CACHE_REFERENCES		2
#define PERF_COUNT_HW_CACHE_MISSES		3
#define PERF_COUNT_HW_BRANCH_INSTRUCTIONS	4
#define PERF_COUNT_HW_BRANCH_MISSES		5

#define PERF_EVENT_IOC_ENABLE		0x2400
#define PERF_EVENT_IOC_RESET		0x2403
#define PERF_IOC_FLAG_GROUP		1

#define PERF_FORMAT_TOTAL_TIME_ENABLED	(1ULL << 0)
#define PERF_FORMAT_TOTAL_TIME_RUNNING	(1ULL << 1)
#define PERF_FORMAT_GROUP		(1ULL << 3)

#define PERF_ATTR_FLAG_DISABLED		(1ULL << 0)
#define PERF_ATTR_FLAG_EXCLUDE_KERNEL	(1ULL << 5)
#define PERF_ATTR_FLAG_EXCLUDE_HV	(1ULL << 6)

#define PERF_ATTR_SIZE_VER0		64

struct perf_event_attr {
	uint32_t type;
	uint32_t size;
	uint64_t config;
	uint64_t sample_period;
	uint64_t sample_type;
	uint64_t read_format;
	uint64_t flags;
	uint32_t wakeup_events;
	uint32_t bp_type;
	uint64_t config1;
};

/* ARMv8 PMU event codes */
#define ARMV8_PMU_MEM_ACCESS		0x06
#define ARMV8_PMU_MEM_ACCESS_READ	0x04
#define ARMV8_PMU_MEM_ACCESS_WRITE	0x05

/* perf_event_open syscall number */
#ifndef __NR_perf_event_open
#ifdef __aarch64__
#define __NR_perf_event_open 241
#else
#define __NR_perf_event_open 364
#endif
#endif

static struct pmu_event pmu_events[MAX_PMU_EVENTS];
static int pmu_event_count;
static int pmu_cpu_count;
static int pmu_cpu_ids[MAX_PRESENT_CPUS];
static int pmu_fds[MAX_PRESENT_CPUS][MAX_PMU_EVENTS];
static uint64_t pmu_prev_raw[MAX_PRESENT_CPUS][MAX_PMU_EVENTS];
static uint64_t pmu_scaled_totals[MAX_PRESENT_CPUS][MAX_PMU_EVENTS];
static uint64_t pmu_prev_time_enabled[MAX_PRESENT_CPUS];
static uint64_t pmu_prev_time_running[MAX_PRESENT_CPUS];
static int pmu_has_baseline[MAX_PRESENT_CPUS];
static char *pmu_event_spec;

static char *trim_event_name(char *event)
{
	char *end;

	while (*event == ' ' || *event == '\t')
		event++;

	if (*event == '\0')
		return event;

	end = event + strlen(event) - 1;
	while (end > event && (*end == ' ' || *end == '\t')) {
		*end = '\0';
		end--;
	}

	return event;
}

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
			    int cpu, int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, hw_event, pid, cpu,
		       group_fd, flags);
}

static int perf_event_open_compat(struct perf_event_attr *attr, int cpu_id,
				  int group_fd)
{
	struct perf_event_attr local_attr;
	long fd;

	local_attr = *attr;
	fd = perf_event_open(&local_attr, -1, cpu_id, group_fd, 0);
	if (fd >= 0)
		return (int)fd;

	/*
	 * Older kernels reject unsupported perf_event_attr sizes with E2BIG and
	 * overwrite attr.size with the size they understand. Retry with that
	 * kernel-reported size so PMU monitoring still works on older ARM
	 * server kernels.
	 */
	if (errno == E2BIG && local_attr.size > 0 &&
	    local_attr.size < sizeof(local_attr)) {
		unsigned int kernel_size = local_attr.size;

		memset((char *)&local_attr + kernel_size, 0,
		       sizeof(local_attr) - kernel_size);
		local_attr.size = kernel_size;

		fd = perf_event_open(&local_attr, -1, cpu_id, group_fd, 0);
		if (fd >= 0)
			return (int)fd;
	}

	return -1;
}

struct perf_group_read_data {
	uint64_t nr;
	uint64_t time_enabled;
	uint64_t time_running;
	uint64_t values[MAX_PMU_EVENTS];
};

/*
 * Built-in PMU event catalog.
 *
 * Each entry maps an armstat event name to a perf_event_attr type and config.
 * The table is searched linearly — it is small enough (< 20 entries) that a
 * hash or binary search would be over-engineering.
 */
static const struct pmu_event_catalog {
	const char *name;
	int           type;
	unsigned long long config;
} pmu_catalog[] = {
	/* Hardware events */
	{ "cycles",             PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES          },
	{ "instructions",       PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS        },
	{ "cache-references",   PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES    },
	{ "cache-misses",       PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES        },
	{ "branches",           PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS },
	{ "branch-misses",      PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES       },
	/* ARMv8 raw events */
	{ "mem-access",         PERF_TYPE_RAW, ARMV8_PMU_MEM_ACCESS        },
	{ "mem-read",           PERF_TYPE_RAW, ARMV8_PMU_MEM_ACCESS_READ   },
	{ "mem-write",          PERF_TYPE_RAW, ARMV8_PMU_MEM_ACCESS_WRITE  },
	{ "l1d-cache-refill",   PERF_TYPE_RAW, 0x03 },
	{ "l1d-cache",          PERF_TYPE_RAW, 0x04 },
	{ "l1i-cache-refill",   PERF_TYPE_RAW, 0x01 },
	{ "l1i-cache",          PERF_TYPE_RAW, 0x02 },
	{ "l2d-cache-refill",   PERF_TYPE_RAW, 0x09 },
	{ "l2d-cache",          PERF_TYPE_RAW, 0x0A },
	{ "l3d-cache-refill",   PERF_TYPE_RAW, 0x13 },
	{ "l3d-cache",          PERF_TYPE_RAW, 0x14 },
};
#define PMU_CATALOG_SIZE ((int)(sizeof(pmu_catalog) / sizeof(pmu_catalog[0])))

static int lookup_arm_event(const char *name, int *type,
			    unsigned long long *config)
{
	for (int i = 0; i < PMU_CATALOG_SIZE; i++) {
		if (strcmp(name, pmu_catalog[i].name) == 0) {
			if (type)
				*type = pmu_catalog[i].type;
			if (config)
				*config = pmu_catalog[i].config;
			return 0;
		}
	}
	return -1;
}

static int resolve_pmu_event(const char *event, int *type,
			     unsigned long long *config)
{
	char tail;

	if (!event || !config)
		return -1;

	if (lookup_arm_event(event, type, config) == 0)
		return 0;

	if (sscanf(event, "0x%llx%c", config, &tail) == 1) {
		if (type)
			*type = PERF_TYPE_RAW;
		return 0;
	}

	return -1;
}

static int pmu_event_list_has_empty_token(const char *events)
{
	int need_value = 1;

	for (const char *p = events; p && *p; p++) {
		if (*p == ',') {
			if (need_value)
				return 1;
			need_value = 1;
			continue;
		}
		if (*p != ' ' && *p != '\t')
			need_value = 0;
	}

	return need_value;
}

static void clear_pmu_metadata(void)
{
	for (int i = 0; i < pmu_event_count; i++) {
		if (pmu_events[i].name) {
			free((void *)pmu_events[i].name);
			pmu_events[i].name = NULL;
		}
		pmu_events[i].fd = -1;
		pmu_events[i].type = 0;
		pmu_events[i].config = 0;
		pmu_events[i].value = 0;
		pmu_events[i].prev_value = 0;
	}

	pmu_event_count = 0;
}

static int parse_pmu_event_list_internal(const char *events, int populate)
{
	char *event_list;
	char *event;
	char *saveptr = NULL;
	int idx = 0;

	if (!events || !*events || pmu_event_list_has_empty_token(events)) {
		fprintf(stderr, "Error: PMU event list must not be empty\n");
		return -1;
	}

	event_list = strdup(events);
	if (!event_list)
		return -1;

	if (populate)
		pmu_event_count = 0;

	event = strtok_r(event_list, ",", &saveptr);
	while (event) {
		int type;
		unsigned long long config;

		event = trim_event_name(event);
		if (*event == '\0') {
			fprintf(stderr, "Error: PMU event list contains an empty token\n");
			goto fail;
		}
		if (idx >= MAX_PMU_EVENTS) {
			fprintf(stderr, "Error: too many PMU events (maximum %d)\n",
				MAX_PMU_EVENTS);
			goto fail;
		}
		if (resolve_pmu_event(event, &type, &config) < 0) {
			fprintf(stderr, "Error: unknown PMU event '%s'\n", event);
			goto fail;
		}

		if (populate) {
			pmu_events[idx].name = strdup(event);
			if (!pmu_events[idx].name)
				goto fail;
			pmu_events[idx].fd = -1;
			pmu_events[idx].type = type;
			pmu_events[idx].config = config;
			pmu_events[idx].value = 0;
			pmu_events[idx].prev_value = 0;
			pmu_event_count = idx + 1;
		}

		idx++;
		event = strtok_r(NULL, ",", &saveptr);
	}

	free(event_list);
	if (idx <= 0) {
		fprintf(stderr, "Error: PMU event list must not be empty\n");
		if (populate)
			clear_pmu_metadata();
		return -1;
	}

	return 0;

fail:
	free(event_list);
	if (populate)
		clear_pmu_metadata();
	return -1;
}

int validate_pmu_event_list(const char *events)
{
	return parse_pmu_event_list_internal(events, 0);
}

void list_builtin_pmu_events(void)
{
	for (int i = 0; i < PMU_CATALOG_SIZE; i++)
		printf("  %s\n", pmu_catalog[i].name);
}

static void clear_pmu_fds(void)
{
	for (int cpu = 0; cpu < pmu_cpu_count; cpu++) {
		for (int event = 0; event < pmu_event_count; event++) {
			if (pmu_fds[cpu][event] >= 0) {
				close(pmu_fds[cpu][event]);
				pmu_fds[cpu][event] = -1;
			}
		}
	}

	pmu_cpu_count = 0;
	memset(pmu_cpu_ids, 0, sizeof(pmu_cpu_ids));
}

static void clear_pmu_runtime_state(void)
{
	memset(pmu_prev_raw, 0, sizeof(pmu_prev_raw));
	memset(pmu_scaled_totals, 0, sizeof(pmu_scaled_totals));
	memset(pmu_prev_time_enabled, 0, sizeof(pmu_prev_time_enabled));
	memset(pmu_prev_time_running, 0, sizeof(pmu_prev_time_running));
	memset(pmu_has_baseline, 0, sizeof(pmu_has_baseline));
}

static void reset_pmu_state(void)
{
	clear_pmu_fds();
	clear_pmu_runtime_state();
	clear_pmu_metadata();

	if (pmu_event_spec) {
		free(pmu_event_spec);
		pmu_event_spec = NULL;
	}
}

static void clear_pmu_open_state(void)
{
	clear_pmu_fds();
	clear_pmu_runtime_state();
}

static int parse_pmu_event_list(const char *events)
{
	return parse_pmu_event_list_internal(events, 1);
}

static int open_pmu_fd_for_cpu(const struct pmu_event *event, int cpu_id,
			       int group_fd)
{
	struct perf_event_attr pe;

	memset(&pe, 0, sizeof(pe));
	pe.type = event->type;
	pe.size = PERF_ATTR_SIZE_VER0;
	pe.config = event->config;
	/*
	 * Match perf group semantics: the leader starts disabled and controls
	 * the group, members join enabled and are toggled by leader ioctls.
	 */
	if (group_fd == -1)
		pe.flags |= PERF_ATTR_FLAG_DISABLED;

	/*
	 * turbostat's perf counters are system-observation counters, not a
	 * userspace-only profile. Keep kernel/hypervisor included here so the
	 * counter domain matches the CPU execution domain the rest of armstat
	 * reports on.
	 */
	pe.read_format = PERF_FORMAT_GROUP |
			 PERF_FORMAT_TOTAL_TIME_ENABLED |
			 PERF_FORMAT_TOTAL_TIME_RUNNING;

	return perf_event_open_compat(&pe, cpu_id, group_fd);
}

static uint64_t scale_pmu_value(uint64_t raw, uint64_t time_enabled,
				uint64_t time_running)
{
	long double scaled;

	if (!time_running)
		return 0;

	if (time_running >= time_enabled || time_enabled == 0)
		return raw;

	scaled = (long double)raw * (long double)time_enabled;
	scaled /= (long double)time_running;

	if (scaled >= (long double)ULLONG_MAX)
		return ULLONG_MAX;

	return (uint64_t)(scaled + 0.5L);
}

static int open_pmu_counters_for_tracked_cpus(void)
{
	int tracked = get_tracked_cpu_count();

	memset(pmu_fds, 0xff, sizeof(pmu_fds));
	pmu_cpu_count = 0;

	for (int cpu_idx = 0; cpu_idx < tracked && cpu_idx < MAX_PRESENT_CPUS; cpu_idx++) {
		int cpu_id = get_cpu_id_by_tracked_idx(cpu_idx);
		int leader_fd = -1;

		if (cpu_id < 0)
			continue;

		for (int event = 0; event < pmu_event_count; event++) {
			int group_fd = (event == 0) ? -1 : leader_fd;
			int fd = open_pmu_fd_for_cpu(&pmu_events[event], cpu_id,
						     group_fd);

			if (fd < 0) {
				fprintf(stderr,
					"perf_event_open for '%s' on CPU %d failed: %s\n",
					pmu_events[event].name ? pmu_events[event].name : "event",
					cpu_id, strerror(errno));
				pmu_cpu_count++;
				clear_pmu_fds();
				return -1;
			}

			pmu_fds[pmu_cpu_count][event] = fd;
			if (event == 0)
				leader_fd = fd;
		}

		if (leader_fd >= 0) {
			ioctl(leader_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
			ioctl(leader_fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
		}

		pmu_cpu_ids[pmu_cpu_count] = cpu_id;
		pmu_cpu_count++;
	}

	return pmu_cpu_count > 0 ? 0 : -1;
}

int init_pmu_events(const char *events)
{
	reset_pmu_state();

	if (parse_pmu_event_list(events) < 0)
		return -1;

	pmu_event_spec = strdup(events);
	if (!pmu_event_spec) {
		clear_pmu_metadata();
		return -1;
	}

	if (open_pmu_counters_for_tracked_cpus() < 0) {
		/*
		 * Preserve parsed event metadata so the formatter can still render
		 * the requested PMU columns as unavailable instead of making them
		 * disappear entirely. Runtime state and fds are dropped.
		 */
		clear_pmu_open_state();
		return -1;
	}

	return 0;
}

int rebuild_pmu_events(void)
{
	if (!pmu_event_spec || pmu_event_count <= 0)
		return 0;

	clear_pmu_fds();
	clear_pmu_runtime_state();
	return open_pmu_counters_for_tracked_cpus();
}

int read_all_pmu_counters(uint64_t (*values)[MAX_PMU_EVENTS], int max_cpus)
{
	if (!values)
		return -1;

	for (int cpu = 0; cpu < max_cpus; cpu++) {
		for (int event = 0; event < pmu_event_count; event++)
			values[cpu][event] = 0;
	}

	for (int cpu = 0; cpu < pmu_cpu_count && cpu < max_cpus; cpu++) {
		struct perf_group_read_data group_data;
		ssize_t expected_size;
		ssize_t ret;

		if (pmu_fds[cpu][0] < 0)
			continue;

		memset(&group_data, 0, sizeof(group_data));
		expected_size = (ssize_t)(sizeof(uint64_t) * (3 + pmu_event_count));
		ret = read(pmu_fds[cpu][0], &group_data, expected_size);
		if (ret < expected_size)
			continue;

		if (!pmu_has_baseline[cpu]) {
			pmu_prev_time_enabled[cpu] = group_data.time_enabled;
			pmu_prev_time_running[cpu] = group_data.time_running;
			for (int event = 0; event < pmu_event_count &&
					     event < (int)group_data.nr; event++)
				pmu_prev_raw[cpu][event] = group_data.values[event];
			pmu_has_baseline[cpu] = 1;
			continue;
		}

		for (int event = 0; event < pmu_event_count &&
				     event < (int)group_data.nr; event++) {
			uint64_t current_raw = group_data.values[event];
			uint64_t previous_raw = pmu_prev_raw[cpu][event];
			uint64_t delta_raw = 0;
			uint64_t delta_enabled = 0;
			uint64_t delta_running = 0;

			if (current_raw >= previous_raw)
				delta_raw = current_raw - previous_raw;
			if (group_data.time_enabled >= pmu_prev_time_enabled[cpu])
				delta_enabled = group_data.time_enabled -
						pmu_prev_time_enabled[cpu];
			if (group_data.time_running >= pmu_prev_time_running[cpu])
				delta_running = group_data.time_running -
						pmu_prev_time_running[cpu];

			pmu_scaled_totals[cpu][event] +=
				scale_pmu_value(delta_raw, delta_enabled, delta_running);
			values[cpu][event] = pmu_scaled_totals[cpu][event];
			pmu_prev_raw[cpu][event] = current_raw;
		}

		pmu_prev_time_enabled[cpu] = group_data.time_enabled;
		pmu_prev_time_running[cpu] = group_data.time_running;
	}

	return 0;
}

void close_pmu_events(void)
{
	reset_pmu_state();
}

int get_pmu_event_count(void)
{
	return pmu_event_count;
}

const char *get_pmu_event_name(int index)
{
	if (index < 0 || index >= pmu_event_count)
		return NULL;

	return pmu_events[index].name;
}

int pmu_is_active(void)
{
	return pmu_event_count > 0 && pmu_cpu_count > 0;
}
