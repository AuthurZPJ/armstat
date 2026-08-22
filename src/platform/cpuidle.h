/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_CPUIDLE_H
#define ARMSTAT_CPUIDLE_H

#include <stdint.h>

#define MAX_IDLE_STATES	16

struct idle_state {
	char name[32];
	unsigned long long time;	/* microseconds */
	unsigned long long usage;	/* entry count (cumulative) */
	double percentage;
	double usage_per_sec;		/* stateN/usage delta per second */
	int time_valid;
	int usage_valid;
	int available;
	int disabled;
};

/* Get number of idle states for a CPU */
int get_idle_state_count(int cpu);

/* Initialize cpuidle subsystem */
int init_cpuidle(void);

/* Close cpuidle subsystem */
void close_cpuidle(void);

/*
 * Enable/disable cpuidle monitoring
 * armstat prefers cpuidle when it is available; disabling it forces the
 * collector to fall back to /proc/stat for Idle%/Busy%.
 */
void enable_cpuidle(int enable);

/* Check if cpuidle is enabled */
int is_cpuidle_enabled(void);

/* Update idle states for all CPUs */
void update_idle_states(unsigned long long elapsed_us);

/*
 * Refresh cached cpuidle disable bits for a limited number of tracked CPUs.
 * This is a slow-changing configuration refresh and is intentionally budgeted
 * so it can be spread across intervals without causing periodic spikes.
 */
void refresh_idle_state_disable_cache_budgeted(int tracked_cpu_budget);

/* Get idle state count for all CPUs */
int get_global_idle_state_count(void);

/* Get idle state name by index (returns NULL if not available) */
const char *get_idle_state_name(int state_idx);

/* Get pointer to internal idle_states array (must call update_idle_states first) */
struct idle_state **get_idle_states_array(void);

/*
 * Set effective tracked CPU count - called by collector to avoid reading
 * beyond MAX_CPUS. This also seeds the cached CPU-ID array size so cpuidle
 * does not need to re-probe it repeatedly.
 */
void set_effective_cpu_count(int count);

#endif /* ARMSTAT_CPUIDLE_H */
