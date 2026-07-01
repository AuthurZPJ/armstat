/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM PMU event monitoring interface
 *
 * Provides functions for monitoring CPU performance monitoring unit (PMU)
 * events through Linux perf_event_open() syscall.
 */

#ifndef ARMSTAT_PMU_H
#define ARMSTAT_PMU_H

#include <stdint.h>
#include "collector.h"

struct pmu_event {
	const char *name;
	int fd;
	int type;
	unsigned long long config;
	uint64_t value;
	uint64_t prev_value;
};

/* Initialize PMU events */
int init_pmu_events(const char *events);

/* Validate a comma-separated event list without opening perf fds. */
int validate_pmu_event_list(const char *events);

/* Print built-in PMU event names. */
void list_builtin_pmu_events(void);

/* Read PMU counters */
int read_all_pmu_counters(uint64_t (*values)[MAX_PMU_EVENTS], int max_cpus);

/* Rebuild PMU file descriptors after CPU hotplug */
int rebuild_pmu_events(void);

/* Check whether PMU counters are active */
int pmu_is_active(void);

/* Close PMU events */
void close_pmu_events(void);

/* Get number of PMU events */
int get_pmu_event_count(void);

/* Get PMU event name by index */
const char *get_pmu_event_name(int index);

#endif /* ARMSTAT_PMU_H */
