/* SPDX-License-Identifier: GPL-2.0 */
/*
 * idle_backend.c - Busy/Idle source policy helpers
 *
 * Busy/Idle percentages are computed from raw counters in sample_cache.c and
 * aggregator.c. This module only centralizes policy decisions:
 *   - selected busy-source mode
 *   - nohz_full CPU mask parsing
 *   - per-CPU choice between procstat and schedstat
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "idle_backend.h"
#include "collector.h"
#include "cpu_inventory.h"

#define ARRAY_SIZE(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

static enum busy_source_mode global_busy_source_mode = BUSY_SOURCE_AUTO;
static unsigned char nohz_full_cpus[MAX_CPUS];
static int nohz_full_valid;
static int nohz_full_count;

static void refresh_nohz_full_mask(void)
{
	FILE *fp;
	char *line = NULL;
	size_t line_size = 0;

	memset(nohz_full_cpus, 0, sizeof(nohz_full_cpus));
	nohz_full_count = 0;

	fp = fopen("/sys/devices/system/cpu/nohz_full", "r");
	if (!fp) {
		nohz_full_valid = 1;
		return;
	}

	if (getline(&line, &line_size, fp) > 0) {
		char *nl = strchr(line, '\n');

		if (nl)
			*nl = '\0';
		if (parse_cpu_list_mask(line, nohz_full_cpus,
					ARRAY_SIZE(nohz_full_cpus),
					&nohz_full_count) < 0)
			nohz_full_count = 0;
	}

	free(line);
	fclose(fp);
	nohz_full_valid = 1;
}

static int should_use_schedstat(int cpu_id)
{
	if (cpu_id < 0 || cpu_id >= MAX_CPUS)
		return 0;

	if (!nohz_full_valid)
		refresh_nohz_full_mask();

	switch (global_busy_source_mode) {
	case BUSY_SOURCE_SCHEDSTAT:
	case BUSY_SOURCE_TASK_CLOCK:
		return 1;
	case BUSY_SOURCE_PROCSTAT:
		return 0;
	case BUSY_SOURCE_AUTO:
	default:
		return nohz_full_cpus[cpu_id] ? 1 : 0;
	}
}

void set_busy_source_mode(enum busy_source_mode mode)
{
	global_busy_source_mode = mode;
	nohz_full_valid = 0;
}

enum busy_source_mode get_busy_source_mode(void)
{
	return global_busy_source_mode;
}

const char *get_busy_source_mode_name(void)
{
	switch (global_busy_source_mode) {
	case BUSY_SOURCE_PROCSTAT:
		return "procstat";
	case BUSY_SOURCE_SCHEDSTAT:
		return "schedstat";
	case BUSY_SOURCE_TASK_CLOCK:
		return "task-clock";
	case BUSY_SOURCE_AUTO:
	default:
		return "auto";
	}
}

const char *get_busy_source_effective_name(void)
{
	switch (global_busy_source_mode) {
	case BUSY_SOURCE_PROCSTAT:
		return "procstat";
	case BUSY_SOURCE_SCHEDSTAT:
	case BUSY_SOURCE_TASK_CLOCK:
		return "schedstat";
	case BUSY_SOURCE_AUTO:
	default:
		return "auto(procstat+schedstat-on-nohz_full)";
	}
}

int busy_source_uses_schedstat_cpu(int cpu_id)
{
	return should_use_schedstat(cpu_id);
}

int cpu_is_nohz_full(int cpu_id)
{
	if (cpu_id < 0 || cpu_id >= MAX_CPUS)
		return 0;
	if (!nohz_full_valid)
		refresh_nohz_full_mask();
	return nohz_full_cpus[cpu_id] ? 1 : 0;
}

int get_nohz_full_cpu_count(void)
{
	if (!nohz_full_valid)
		refresh_nohz_full_mask();
	return nohz_full_count;
}
