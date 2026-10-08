/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_CPUIDLE_H
#define ARMSTAT_CPUIDLE_H

#include <stdint.h>

#define MAX_IDLE_STATES	16

struct idle_state {
	unsigned long long time;	/* microseconds */
	unsigned long long usage;	/* entry count (cumulative) */
	double percentage;		/* time delta / interval * 100, may exceed 100 */
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
 * This controls per-state residency and usage only. Busy/Idle uses its own
 * procstat/schedstat policy regardless of cpuidle availability.
 */
void enable_cpuidle(int enable);

/* Check if cpuidle is enabled */
int is_cpuidle_enabled(void);

/*
 * Pure interval helpers. Invalid baselines, zero-length intervals, and counter
 * resets return NAN. Preserve residency above 100% from delayed state-exit
 * accounting; do not normalize it to Busy/Idle.
 */
double cpuidle_residency_percent(unsigned long long current, int current_valid,
				 unsigned long long previous, int previous_valid,
				 unsigned long long elapsed_us);
double cpuidle_usage_per_sec(unsigned long long current, int current_valid,
			     unsigned long long previous, int previous_valid,
			     unsigned long long elapsed_us);

/* Update only the selected state counters for all tracked CPUs. */
void update_idle_states(unsigned long long elapsed_us,
			unsigned int residency_mask,
			unsigned int usage_mask);

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
