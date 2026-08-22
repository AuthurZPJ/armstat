/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM CPU idle state monitoring
 *
 * Two-layer architecture for performance:
 *
 * 1. STATIC LAYER (init/rescan):
 *    - State names (e.g., "LPI-0", "LPI-1", "WFI")
 *    - State paths
 *
 * 2. DYNAMIC LAYER (per-interval):
 *    - State time counters
 *
 * DEFAULT POLICY:
 *    Use /proc/stat for authoritative Idle%/Busy% so short-interval scheduler
 *    busy time matches tools such as htop more closely. cpuidle is still used
 *    for split ARM idle residency because state names (for example LPI-0 /
 *    LPI-1) carry useful platform semantics that /proc/stat cannot provide.
 *
 * Idle states (per CPU) are named from sysfs stateN/name and should be shown
 * using those names directly (for example "LPI-0", "LPI-1"), rather than
 * synthetic x86-like C-state labels.
 *
 * Data source:
 *   /sys/devices/system/cpu/cpuN/cpuidle/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>

#include "cpuidle.h"
#include "cpu_inventory.h"
#include "sysfs_util.h"

/*
 * Idle state tracking
 * Stores cumulative time in each idle state
 */

/*
 * Prefer split cpuidle residency when available. Platforms without cpuidle
 * simply lose the LPI-* breakdown; authoritative Busy/Idle still comes from
 * the procstat/schedstat raw-counter path.
 */
static int cpuidle_enabled = 1;
static int cpuidle_initialized = 0;  /* Track if init has been called */

static int max_idle_states;
static struct idle_state **idle_states;
static unsigned long long *prev_state_times;  /* Per-CPU previous state times */
static unsigned long long *prev_state_usages; /* Per-CPU previous state usage */
static unsigned char *prev_state_time_valid;
static unsigned char *prev_state_usage_valid;
static int *cpu_idle_state_counts;
static unsigned char *cpu_idle_state_disabled;
static int *state_time_fds;
static int disable_refresh_cursor;
static int state_time_fd_cap;
static int state_time_fd_open_count;

/*
 * PMU monitoring can keep hundreds of perf_event fds open on large machines.
 * Keep cpuidle's persistent cache intentionally small so split-LPI reporting
 * never consumes the entire process fd budget.
 */
#define MAX_STATE_TIME_FD_CACHE 32

/* STATIC LAYER: Cached at init, never re-read */
/* State names - read once at init, static throughout runtime */
static char (*idle_state_names)[32];

/* Cached CPU-ID array size - set once at init, never probe it repeatedly */
static int cached_cpu_count = 0;
static int effective_cpu_count = 0;
static int get_cached_cpu_count(void);

static int state_fd_index(int cpu, int state)
{
	return cpu * max_idle_states + state;
}

static int get_state_time_fd(int tracked_idx, int state)
{
	char path[256];
	char sub[64];
	int idx;
	int fd;
	int cpu_id;

	if (!state_time_fds)
		return -1;

	idx = state_fd_index(tracked_idx, state);
	fd = state_time_fds[idx];
	if (fd >= 0)
		return fd;

	/*
	 * On large-core servers, cpuidle state-time fds are only one part of the
	 * process-wide fd footprint; PMU perf groups and cpufreq caches also keep
	 * descriptors open. Cap the persistent cpuidle cache so enabling PMU does
	 * not starve later sysfs/proc reads and collapse idle accounting.
	 */
	if (state_time_fd_open_count >= state_time_fd_cap)
		return -1;

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (cpu_id < 0)
		return -1;

	snprintf(sub, sizeof(sub), "cpuidle/state%d/time", state);
	if (cpu_sysfs_path(cpu_id, sub, path, sizeof(path)) < 0)
		return -1;
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		state_time_fds[idx] = fd;
		state_time_fd_open_count++;
	}

	return fd;
}

static void copy_idle_state_name(char dest[32], const char *src)
{
	size_t len;

	if (!src) {
		dest[0] = '\0';
		return;
	}

	len = strnlen(src, 31);
	memcpy(dest, src, len);
	dest[len] = '\0';
}

static void refresh_disable_bits_for_cpu(int tracked_idx)
{
	char path[256];
	char sub[64];
	int cpu_id;

	if (tracked_idx < 0 ||
	    !cpu_idle_state_counts || !cpu_idle_state_disabled)
		return;

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (cpu_id < 0)
		return;

	for (int state = 0; state < cpu_idle_state_counts[tracked_idx]; state++) {
		unsigned long long disable = 0;

		snprintf(sub, sizeof(sub), "cpuidle/state%d/disable", state);
		if (cpu_sysfs_path(cpu_id, sub, path, sizeof(path)) < 0)
			continue;
		if (sysfs_read_ull_checked(path, &disable) == 0)
			cpu_idle_state_disabled[tracked_idx * max_idle_states + state] =
				disable ? 1 : 0;
		/* Preserve the last known disable bit on a transient read failure. */
	}
}

int get_idle_state_count(int cpu)
{
	DIR *dir;
	struct dirent *entry;
	char path[256];
	int count = 0;

	if (cpu < 0 || cpu_sysfs_path(cpu, "cpuidle", path, sizeof(path)) < 0)
		return -1;

	dir = opendir(path);
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		int state;
		char tail;

		if (sscanf(entry->d_name, "state%d%c", &state, &tail) == 1 &&
		    state >= 0 && state < MAX_IDLE_STATES && state + 1 > count)
			count = state + 1;
	}

	closedir(dir);
	return count;
}

int get_global_idle_state_count(void)
{
	int ref_cpu;

	if (!cpuidle_enabled || !cpuidle_initialized)
		return 0;

	if (max_idle_states > 0)
		return max_idle_states;

	ref_cpu = get_cpu_id_by_tracked_idx(0);
	if (ref_cpu < 0)
		ref_cpu = 0;

	return get_idle_state_count(ref_cpu);
}

const char *get_idle_state_name(int state_idx)
{
	if (state_idx < 0 || state_idx >= max_idle_states)
		return NULL;
	if (!idle_state_names || !idle_state_names[state_idx][0])
		return NULL;
	return idle_state_names[state_idx];
}

/*
 * Set effective CPU count - called by collector to avoid reading beyond MAX_CPUS
 * This also updates cached_cpu_count to avoid repeated array-size probes.
 */
void set_effective_cpu_count(int count)
{
	effective_cpu_count = count;
	if (cached_cpu_count == 0)
		cached_cpu_count = count;
}

/*
 * Get cached CPU-ID array size (avoid repeated probes)
 */
static int get_cached_cpu_count(void)
{
	if (cached_cpu_count > 0)
		return cached_cpu_count;
	/* Fallback to the dense tracked count if not set */
	cached_cpu_count = get_tracked_cpu_count();
	return cached_cpu_count > 0 ? cached_cpu_count : 1;
}

/*
 * get_idle_states_array - Get pointer to internal idle_states array
 * Must be called after update_idle_states() to get calculated percentages
 */
struct idle_state **get_idle_states_array(void)
{
	return idle_states;
}

static int read_idle_state(int tracked_idx, int state, struct idle_state *info)
{
	char path[256];
	char sub[64];
	int fd;
	int cpu_id;

	memset(info, 0, sizeof(*info));
	info->percentage = NAN;
	info->wakeups_per_sec = NAN;

	if (tracked_idx < 0 || state < 0 ||
	    !cpu_idle_state_counts ||
	    state >= cpu_idle_state_counts[tracked_idx]) {
		info->available = 0;
		info->disabled = 0;
		return -1;
	}

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (cpu_id < 0) {
		info->available = 0;
		info->disabled = 0;
		return -1;
	}

	info->available = 1;

	/* STATIC LAYER: Use cached name (read once at init) */
	if (idle_state_names && state < max_idle_states && idle_state_names[state][0]) {
		strncpy(info->name, idle_state_names[state], sizeof(info->name) - 1);
		info->name[sizeof(info->name) - 1] = 0;
	} else {
		/* Should never happen if init succeeded - use fallback */
		info->name[0] = '\0';
	}

	/* DYNAMIC LAYER: Read time and usage every interval */
	fd = get_state_time_fd(tracked_idx, state);
	if (fd >= 0) {
		if (fd_read_ull_checked(fd, &info->time) == 0) {
			info->time_valid = 1;
		} else {
			int idx = state_fd_index(tracked_idx, state);

			close(fd);
			state_time_fds[idx] = -1;
			state_time_fd_open_count--;
		}
	}
	if (!info->time_valid) {
		snprintf(sub, sizeof(sub), "cpuidle/state%d/time", state);
		if (cpu_sysfs_path(cpu_id, sub, path, sizeof(path)) == 0 &&
		    sysfs_read_ull_checked(path, &info->time) == 0)
			info->time_valid = 1;
	}

	/* usage is in a separate file and is not part of the cached-fd budget. */
	snprintf(sub, sizeof(sub), "cpuidle/state%d/usage", state);
	if (cpu_sysfs_path(cpu_id, sub, path, sizeof(path)) == 0 &&
	    sysfs_read_ull_checked(path, &info->usage) == 0)
		info->usage_valid = 1;

	if (cpu_idle_state_disabled)
		info->disabled = cpu_idle_state_disabled[tracked_idx * max_idle_states + state];
	else
		info->disabled = 0;

	return 0;
}

static void free_idle_state_matrix(int total_cpus)
{
	if (!idle_states)
		return;

	for (int cpu = 0; cpu < total_cpus; cpu++) {
		free(idle_states[cpu]);
		idle_states[cpu] = NULL;
	}

	free(idle_states);
	idle_states = NULL;
}

static void cleanup_cpuidle_runtime_allocations(int total_cpus)
{
	if (state_time_fds) {
		for (int i = 0; i < total_cpus * max_idle_states; i++) {
			if (state_time_fds[i] >= 0)
				close(state_time_fds[i]);
		}
		free(state_time_fds);
		state_time_fds = NULL;
	}

	if (cpu_idle_state_disabled) {
		free(cpu_idle_state_disabled);
		cpu_idle_state_disabled = NULL;
	}
	if (cpu_idle_state_counts) {
		free(cpu_idle_state_counts);
		cpu_idle_state_counts = NULL;
	}
	if (prev_state_times) {
		free(prev_state_times);
		prev_state_times = NULL;
	}
	if (prev_state_usages) {
		free(prev_state_usages);
		prev_state_usages = NULL;
	}
	if (prev_state_time_valid) {
		free(prev_state_time_valid);
		prev_state_time_valid = NULL;
	}
	if (prev_state_usage_valid) {
		free(prev_state_usage_valid);
		prev_state_usage_valid = NULL;
	}

	free_idle_state_matrix(total_cpus);
}

int init_cpuidle(void)
{
	int cpu;
	int tracked;
	int state_count;
	int total_cpus;

	/* Idempotency: avoid leaking prior allocations if called twice */
	if (cpuidle_initialized)
		return 0;

	/*
	 * Arrays are dense, sized by the tracked CPU count — not the sparse
	 * real-ID space. effective_cpu_count is the tracked count set by the
	 * collector before runtime init; trust it as the allocation size.
	 */
	total_cpus = get_tracked_cpu_count();
	if (total_cpus <= 0)
		return -1;
	cached_cpu_count = total_cpus;
	effective_cpu_count = total_cpus;

	tracked = total_cpus;
	state_count = 0;
	for (int tracked_idx = 0; tracked_idx < tracked; tracked_idx++) {
		int cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
		int cpu_state_count;

		if (cpu_id < 0)
			continue;

		cpu_state_count = get_idle_state_count(cpu_id);
		if (cpu_state_count > state_count)
			state_count = cpu_state_count;
	}

	if (state_count <= 0)
		return -1;

	max_idle_states = state_count;

	idle_states = calloc(total_cpus, sizeof(struct idle_state *));
	if (!idle_states)
		return -1;

	for (cpu = 0; cpu < total_cpus; cpu++) {
		idle_states[cpu] = calloc(max_idle_states,
					  sizeof(struct idle_state));
		if (!idle_states[cpu]) {
			free_idle_state_matrix(cpu);
			return -1;
		}
	}

	/* Allocate per-CPU previous state times */
	prev_state_times = calloc(total_cpus * max_idle_states,
				 sizeof(unsigned long long));
	if (!prev_state_times) {
		free_idle_state_matrix(total_cpus);
		return -1;
	}

	/* Allocate per-CPU previous state usage counters (for wakeup deltas) */
	prev_state_usages = calloc(total_cpus * max_idle_states,
				  sizeof(unsigned long long));
	if (!prev_state_usages) {
		cleanup_cpuidle_runtime_allocations(total_cpus);
		return -1;
	}
	prev_state_time_valid = calloc(total_cpus * max_idle_states,
				       sizeof(unsigned char));
	prev_state_usage_valid = calloc(total_cpus * max_idle_states,
					sizeof(unsigned char));
	if (!prev_state_time_valid || !prev_state_usage_valid) {
		cleanup_cpuidle_runtime_allocations(total_cpus);
		return -1;
	}

	cpu_idle_state_counts = calloc(total_cpus, sizeof(int));
	if (!cpu_idle_state_counts) {
		cleanup_cpuidle_runtime_allocations(total_cpus);
		return -1;
	}
	for (cpu = 0; cpu < total_cpus; cpu++) {
		int cpu_id = get_cpu_id_by_tracked_idx(cpu);

		cpu_idle_state_counts[cpu] = (cpu_id >= 0) ?
					     get_idle_state_count(cpu_id) : 0;
	}

	state_time_fds = malloc(total_cpus * max_idle_states * sizeof(int));
	if (!state_time_fds) {
		cleanup_cpuidle_runtime_allocations(total_cpus);
		return -1;
	}
	for (cpu = 0; cpu < total_cpus * max_idle_states; cpu++)
		state_time_fds[cpu] = -1;

	cpu_idle_state_disabled = calloc(total_cpus * max_idle_states,
					 sizeof(unsigned char));
	if (!cpu_idle_state_disabled) {
		cleanup_cpuidle_runtime_allocations(total_cpus);
		return -1;
	}
	for (cpu = 0; cpu < total_cpus; cpu++)
		refresh_disable_bits_for_cpu(cpu);
	disable_refresh_cursor = 0;
	state_time_fd_open_count = 0;
	state_time_fd_cap = total_cpus * max_idle_states;
	if (state_time_fd_cap > MAX_STATE_TIME_FD_CACHE)
		state_time_fd_cap = MAX_STATE_TIME_FD_CACHE;

	/* STATIC LAYER: Cache idle state names (read once, never changes) */
	idle_state_names = calloc(max_idle_states, sizeof(char[32]));
	if (!idle_state_names) {
		/* Continue without cached names - will read each time */
	} else {
		/*
		 * Read state names once at init.
		 *
		 * If different tracked CPUs expose different names for the same
		 * state index, prefer a generic fallback later rather than baking
		 * one CPU's label into the global header and misleading the user.
		 */
		for (int s = 0; s < max_idle_states; s++) {
			int conflicting_name = 0;
			for (int tracked_idx = 0; tracked_idx < tracked; tracked_idx++) {
				int cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
				char path[256];
				char name_buf[1024];
				char *name;

				if (cpu_id < 0 || get_idle_state_count(cpu_id) <= s)
					continue;

				snprintf(path, sizeof(path),
					 "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/name",
					 cpu_id, s);
				name = sysfs_read_str(path, name_buf,
						      sizeof(name_buf));
				if (!*name)
					continue;

				if (!idle_state_names[s][0]) {
					copy_idle_state_name(idle_state_names[s], name);
					continue;
				}

				if (strncmp(idle_state_names[s], name, sizeof(idle_state_names[s])) != 0) {
					conflicting_name = 1;
					break;
				}
			}

			if (conflicting_name)
				idle_state_names[s][0] = '\0';
		}
	}

	cpuidle_initialized = 1;
	return 0;
}

void update_idle_states(unsigned long long elapsed_us)
{
	int tracked;

	/* Fast path: if cpuidle is not enabled, skip all sysfs reads */
	if (!cpuidle_enabled)
		return;

	tracked = effective_cpu_count > 0 ? effective_cpu_count : get_cached_cpu_count();

	/* Read only tracked CPUs; arrays are dense by tracked index. */
	for (int tracked_idx = 0; tracked_idx < tracked; tracked_idx++) {
		int state;

		if (!idle_states[tracked_idx])
			continue;

		for (state = 0; state < max_idle_states; state++) {
			read_idle_state(tracked_idx, state, &idle_states[tracked_idx][state]);
		}
	}

	/* Calculate interval deltas and per-state residency percentages. */
	for (int tracked_idx = 0; tracked_idx < tracked; tracked_idx++) {
		int state;

		if (!idle_states[tracked_idx])
			continue;

		/*
		 * Calculate per-state residency against the sampled
		 * wall-clock interval. Idle%/Busy% comes from /proc/stat,
		 * while split LPI-* columns come from cpuidle residency.
		 * Keeping the denominator as wall-clock time makes the
		 * split state columns interpretable on their own.
		 */
		for (state = 0; state < max_idle_states; state++) {
			int idx = tracked_idx * max_idle_states + state;
			unsigned long long state_delta;
			unsigned long long usage_delta;

			if (idle_states[tracked_idx][state].time_valid &&
			    prev_state_time_valid[idx] && elapsed_us > 0 &&
			    idle_states[tracked_idx][state].time >= prev_state_times[idx]) {
				state_delta = idle_states[tracked_idx][state].time -
					prev_state_times[idx];
				idle_states[tracked_idx][state].percentage =
					(double)state_delta * 100.0 / (double)elapsed_us;
				if (idle_states[tracked_idx][state].percentage > 100.0)
					idle_states[tracked_idx][state].percentage = 100.0;
			}

			/* Wakeups per second = usage_delta / interval_seconds. */
			if (idle_states[tracked_idx][state].usage_valid &&
			    prev_state_usage_valid[idx] && elapsed_us > 0 &&
			    idle_states[tracked_idx][state].usage >= prev_state_usages[idx]) {
				usage_delta = idle_states[tracked_idx][state].usage -
					prev_state_usages[idx];
				idle_states[tracked_idx][state].wakeups_per_sec =
					(double)usage_delta * 1000000.0 /
					(double)elapsed_us;
			}
		}
	}

	/* Store current values for next iteration */
	for (int tracked_idx = 0; tracked_idx < tracked; tracked_idx++) {
		int state;

		if (!idle_states[tracked_idx])
			continue;

		for (state = 0; state < max_idle_states; state++) {
			int idx = tracked_idx * max_idle_states + state;

			prev_state_times[idx] =
				idle_states[tracked_idx][state].time;
			prev_state_usages[idx] =
				idle_states[tracked_idx][state].usage;
			prev_state_time_valid[idx] =
				idle_states[tracked_idx][state].time_valid ? 1 : 0;
			prev_state_usage_valid[idx] =
				idle_states[tracked_idx][state].usage_valid ? 1 : 0;
		}
	}

}

void refresh_idle_state_disable_cache_budgeted(int tracked_cpu_budget)
{
	int tracked;

	if (!cpuidle_enabled || !cpuidle_initialized ||
	    tracked_cpu_budget <= 0 || effective_cpu_count <= 0)
		return;

	tracked = effective_cpu_count;
	if (tracked <= 0)
		return;

	if (disable_refresh_cursor >= tracked)
		disable_refresh_cursor = 0;

	for (int refreshed = 0; refreshed < tracked_cpu_budget; refreshed++) {
		int tracked_idx = disable_refresh_cursor % tracked;

		refresh_disable_bits_for_cpu(tracked_idx);

		disable_refresh_cursor = (disable_refresh_cursor + 1) % tracked;
	}
}

/*
 * enable_cpuidle - Enable or disable cpuidle monitoring
 *
 * cpuidle can be toggled from the CLI. When enabled, armstat exposes split
 * LPI residency columns; when disabled, only /proc/stat-based Idle%/Busy%
 * remains.
 */
void enable_cpuidle(int enable)
{
	if (!enable) {
		cpuidle_enabled = 0;
		close_cpuidle();
		return;
	}

	if (cpuidle_enabled)
		return;

	cpuidle_enabled = 1;

	/*
	 * During CLI parsing the collector may not have established tracked CPUs
	 * yet. Defer the real init until collector syncs runtime state.
	 */
	if (effective_cpu_count <= 0)
		return;

	close_cpuidle();
	if (init_cpuidle() < 0)
		cpuidle_enabled = 0;
}

/*
 * is_cpuidle_enabled - Check if cpuidle monitoring is enabled
 */
int is_cpuidle_enabled(void)
{
	return cpuidle_enabled;
}

/*
 * close_cpuidle - Free all cpuidle allocated memory
 * Called when CPU hotplug is detected to allow reinit with new CPU count
 */
void close_cpuidle(void)
{
	int i;

	/* Free idle states arrays */
	if (idle_states) {
		for (i = 0; i < cached_cpu_count && idle_states[i]; i++) {
			free(idle_states[i]);
			idle_states[i] = NULL;
		}
		free(idle_states);
		idle_states = NULL;
	}

	/* Free previous state times and usage counters */
	if (prev_state_times) {
		free(prev_state_times);
		prev_state_times = NULL;
	}
	if (prev_state_usages) {
		free(prev_state_usages);
		prev_state_usages = NULL;
	}
	if (prev_state_time_valid) {
		free(prev_state_time_valid);
		prev_state_time_valid = NULL;
	}
	if (prev_state_usage_valid) {
		free(prev_state_usage_valid);
		prev_state_usage_valid = NULL;
	}

	if (cpu_idle_state_counts) {
		free(cpu_idle_state_counts);
		cpu_idle_state_counts = NULL;
	}
	if (cpu_idle_state_disabled) {
		free(cpu_idle_state_disabled);
		cpu_idle_state_disabled = NULL;
	}
	if (state_time_fds) {
		for (i = 0; i < cached_cpu_count * max_idle_states; i++) {
			if (state_time_fds[i] >= 0)
				close(state_time_fds[i]);
		}
		free(state_time_fds);
		state_time_fds = NULL;
	}

	/* Reset counters */
	max_idle_states = 0;
	cached_cpu_count = 0;
	effective_cpu_count = 0;
	disable_refresh_cursor = 0;
	state_time_fd_open_count = 0;
	state_time_fd_cap = 0;

	/* Free static layer caches */
	if (idle_state_names) {
		free(idle_state_names);
		idle_state_names = NULL;
	}
	cpuidle_initialized = 0;
}
