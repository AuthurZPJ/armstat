/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sample_cache.c - Sample cache and fast-path data collection
 *
 * Manages memory pools and per-interval data collection.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>

#include "sample_cache.h"
#include "collector.h"
#include "cpufreq.h"
#include "cpuidle.h"
#include "power.h"
#include "pmu.h"
#include "sysstat.h"
#include "cpu_inventory.h"
#include "idle_backend.h"
#include "columns.h"
#include "formatter_section.h"

/* MAX_CPUS is defined in collector.h — single source of truth */

/* Memory pools */
static struct cpu_freq_info *freqs_pool;
static unsigned long long *authoritative_idle_jiffies_pool;
static unsigned long long *authoritative_iowait_jiffies_pool;
static unsigned char *authoritative_procstat_valid_pool;
static unsigned long long *authoritative_runtime_ns_pool;
static unsigned char *authoritative_runtime_valid_pool;
static uint64_t (*pmu_pool)[MAX_PMU_EVENTS];
static unsigned char *pmu_valid_pool;

static int cache_initialized;

static void pool_free(void);
static void slow_free(void);

/*
 * pool_init - Allocate memory pools
 */
static int pool_init(int count)
{
	int alloc_count = count > 0 ? count : 1;

	freqs_pool = calloc(alloc_count, sizeof(struct cpu_freq_info));
	authoritative_idle_jiffies_pool = calloc(alloc_count, sizeof(unsigned long long));
	authoritative_iowait_jiffies_pool = calloc(alloc_count, sizeof(unsigned long long));
	authoritative_procstat_valid_pool = calloc(alloc_count, sizeof(unsigned char));
	authoritative_runtime_ns_pool = calloc(alloc_count, sizeof(unsigned long long));
	authoritative_runtime_valid_pool = calloc(alloc_count, sizeof(unsigned char));
	pmu_pool = calloc(alloc_count, sizeof(*pmu_pool));
	pmu_valid_pool = calloc(alloc_count, sizeof(unsigned char));

	if (!freqs_pool ||
	    !authoritative_idle_jiffies_pool ||
	    !authoritative_iowait_jiffies_pool ||
	    !authoritative_procstat_valid_pool ||
	    !authoritative_runtime_ns_pool ||
	    !authoritative_runtime_valid_pool ||
	    !pmu_pool || !pmu_valid_pool) {
		pool_free();
		return -1;
	}

	return 0;
}

static void pool_free(void)
{
	if (freqs_pool) { free(freqs_pool); freqs_pool = NULL; }
	if (authoritative_idle_jiffies_pool) {
		free(authoritative_idle_jiffies_pool);
		authoritative_idle_jiffies_pool = NULL;
	}
	if (authoritative_iowait_jiffies_pool) {
		free(authoritative_iowait_jiffies_pool);
		authoritative_iowait_jiffies_pool = NULL;
	}
	if (authoritative_procstat_valid_pool) {
		free(authoritative_procstat_valid_pool);
		authoritative_procstat_valid_pool = NULL;
	}
	if (authoritative_runtime_ns_pool) {
		free(authoritative_runtime_ns_pool);
		authoritative_runtime_ns_pool = NULL;
	}
	if (authoritative_runtime_valid_pool) {
		free(authoritative_runtime_valid_pool);
		authoritative_runtime_valid_pool = NULL;
	}
	if (pmu_pool) { free(pmu_pool); pmu_pool = NULL; }
	if (pmu_valid_pool) { free(pmu_valid_pool); pmu_valid_pool = NULL; }
}

/* Forward declarations for slow layer */
static int slow_layer_initialized;
static int slow_cursor;
static int slow_full_refresh_done;

struct slow_data {
	unsigned int *min_freq;
	unsigned char *min_freq_valid;
	unsigned int *max_freq;
	unsigned char *max_freq_valid;
	int *boost;
	char (*governors)[32];
	int last_cpu_count;
} slow_data;

#define SLOW_TARGET_SWEEP_MS_DEFAULT 5000
#define SLOW_MAX_CPU_BUDGET 32
#define SLOW_MIN_CPU_BUDGET 1
static int slow_target_sweep_ms = SLOW_TARGET_SWEEP_MS_DEFAULT;

static int slow_init(int count)
{
	int alloc_count = count > 0 ? count : 1;

	if (slow_layer_initialized)
		return 0;

	slow_data.min_freq = calloc(alloc_count, sizeof(unsigned int));
	slow_data.min_freq_valid = calloc(alloc_count, sizeof(unsigned char));
	slow_data.max_freq = calloc(alloc_count, sizeof(unsigned int));
	slow_data.max_freq_valid = calloc(alloc_count, sizeof(unsigned char));
	slow_data.boost = calloc(alloc_count, sizeof(int));
	slow_data.governors = calloc(alloc_count, sizeof(char[32]));
	if (!slow_data.min_freq || !slow_data.min_freq_valid ||
	    !slow_data.max_freq || !slow_data.max_freq_valid ||
	    !slow_data.boost || !slow_data.governors) {
		slow_free();
		return -1;
	}

	for (int i = 0; i < count; i++)
		slow_data.boost[i] = -1;

	slow_data.last_cpu_count = count;
	slow_cursor = 0;
	slow_full_refresh_done = 0;

	{
		const char *env = getenv("ARMSTAT_SLOW_SWEEP_MS");
		char *end = NULL;
		long val;

		slow_target_sweep_ms = SLOW_TARGET_SWEEP_MS_DEFAULT;
		if (env) {
			errno = 0;
			val = strtol(env, &end, 10);
			if (!errno && end != env && *end == '\0' &&
			    val >= 100 && val <= 60000)
				slow_target_sweep_ms = (int)val;
		}
	}

	slow_layer_initialized = 1;
	return 0;
}

static void slow_free(void)
{
	if (slow_data.min_freq) { free(slow_data.min_freq); slow_data.min_freq = NULL; }
	if (slow_data.min_freq_valid) {
		free(slow_data.min_freq_valid);
		slow_data.min_freq_valid = NULL;
	}
	if (slow_data.max_freq) { free(slow_data.max_freq); slow_data.max_freq = NULL; }
	if (slow_data.max_freq_valid) {
		free(slow_data.max_freq_valid);
		slow_data.max_freq_valid = NULL;
	}
	if (slow_data.boost) { free(slow_data.boost); slow_data.boost = NULL; }
	if (slow_data.governors) { free(slow_data.governors); slow_data.governors = NULL; }
	memset(&slow_data, 0, sizeof(slow_data));
	slow_cursor = 0;
	slow_full_refresh_done = 0;
	slow_layer_initialized = 0;
}

static int slow_budget_for_interval(int tracked, unsigned long long delta_us)
{
	unsigned long long target_us = (unsigned long long)slow_target_sweep_ms * 1000ULL;
	unsigned long long budget;

	if (tracked <= 0)
		return 0;
	if (delta_us == 0)
		return tracked;

	/*
	 * Spread slow-path refreshes over roughly SLOW_TARGET_SWEEP_MS instead of
	 * rescanning every tracked CPU in one burst. This keeps short sampling
	 * intervals from showing periodic CPU spikes caused by housekeeping work.
	 */
	budget = ((unsigned long long)tracked * delta_us + target_us - 1) / target_us;
	if (budget < SLOW_MIN_CPU_BUDGET)
		budget = SLOW_MIN_CPU_BUDGET;
	if (budget > SLOW_MAX_CPU_BUDGET)
		budget = SLOW_MAX_CPU_BUDGET;
	if (budget > (unsigned long long)tracked)
		budget = tracked;
	return (int)budget;
}

static void slow_refresh_cpu(int tracked_idx)
{
	unsigned int min_freq;
	unsigned int max_freq;
	int min_valid;
	int max_valid;

	if (!is_freq_enabled() || tracked_idx < 0 ||
	    tracked_idx >= get_tracked_cpu_count())
		return;

	read_cpu_min_max_freq_checked(tracked_idx, &min_freq, &min_valid,
				      &max_freq, &max_valid);
	if (min_valid) {
		slow_data.min_freq[tracked_idx] = min_freq;
		slow_data.min_freq_valid[tracked_idx] = 1;
	}
	if (max_valid) {
		slow_data.max_freq[tracked_idx] = max_freq;
		slow_data.max_freq_valid[tracked_idx] = 1;
	}
	/*
	 * Boost is a slow-changing capability/configuration bit. A transient read
	 * failure should not erase a previously known 0/1 value and turn the
	 * column into "-". Preserve the cached value unless we successfully read
	 * a new one.
	 */
	{
		int boost;

		if (read_cpu_boost(tracked_idx, &boost) == 0)
			slow_data.boost[tracked_idx] = boost;
	}

	read_cpu_governor(tracked_idx, slow_data.governors[tracked_idx], 32);
}

static void slow_update_all(int tracked)
{
	for (int i = 0; i < tracked; i++)
		slow_refresh_cpu(i);

	slow_cursor = 0;
	slow_full_refresh_done = 1;
}

static void slow_update_budgeted(int tracked, unsigned long long delta_us)
{
	int budget;

	if (tracked <= 0)
		return;

	if (!slow_full_refresh_done) {
		slow_update_all(tracked);
		return;
	}

	budget = slow_budget_for_interval(tracked, delta_us);
	for (int refreshed = 0; refreshed < budget; refreshed++) {
		if (slow_cursor >= tracked)
			slow_cursor = 0;
		slow_refresh_cpu(slow_cursor);
		slow_cursor++;
	}

}

static void maybe_refresh_cpuidle_disable_cache(int tracked, unsigned long long delta_us)
{
	int budget;

	if (!is_cpuidle_enabled())
		return;

	budget = slow_budget_for_interval(tracked, delta_us);
	if (budget <= 0)
		return;

	refresh_idle_state_disable_cache_budgeted(budget);
}

static void maybe_run_slow_refresh(int tracked, unsigned long long delta_us)
{
	if (!slow_layer_initialized)
		return;

	if (!slow_full_refresh_done) {
		slow_update_all(tracked);
		maybe_refresh_cpuidle_disable_cache(tracked, 0);
		return;
	}

	/*
	 * Run the slow layer after the fast-path snapshot. This keeps the
	 * interval-critical data collection deterministic while spreading
	 * low-frequency configuration refreshes across samples.
	 */
	slow_update_budgeted(tracked, delta_us);
	maybe_refresh_cpuidle_disable_cache(tracked, delta_us);
}

/*
 * collect_sample_data - Fast-path per-interval data collection
 * (internal static version)
 */
static void collect_freq_snapshot(struct sys_snapshot *snapshot, int tracked)
{
	snapshot->freqs = freqs_pool;
	snapshot->uncore_freq_hz = 0;
	snapshot->uncore_freq_valid = 0;
	if (!is_freq_enabled())
		return;

	/*
	 * Uncore/devfreq is a platform-level summary signal. Keep its sampling
	 * independent from the per-CPU cpufreq array so the code reads as
	 * "summary frequency + per-CPU frequencies", not as one monolithic block.
	 */
	if (read_uncore_freq(&snapshot->uncore_freq_hz) == 0)
		snapshot->uncore_freq_valid = 1;

	if (!snapshot->freqs)
		return;

	for (int i = 0; i < tracked; i++) {
		snapshot->freqs[i].cur_freq_valid =
			read_cpu_freq(i, &snapshot->freqs[i].cur_freq) == 0;
		if (!snapshot->freqs[i].cur_freq_valid)
			snapshot->freqs[i].cur_freq = 0;
		if (slow_layer_initialized && slow_data.min_freq) {
			snapshot->freqs[i].min_freq = slow_data.min_freq[i];
			snapshot->freqs[i].min_freq_valid =
				slow_data.min_freq_valid[i];
			snapshot->freqs[i].max_freq = slow_data.max_freq[i];
			snapshot->freqs[i].max_freq_valid =
				slow_data.max_freq_valid[i];
			snapshot->freqs[i].boost = slow_data.boost ? slow_data.boost[i] : -1;
			copy_cstring(snapshot->freqs[i].governor,
				     sizeof(snapshot->freqs[i].governor),
				     slow_data.governors[i]);
		} else {
			snapshot->freqs[i].min_freq = 0;
			snapshot->freqs[i].min_freq_valid = 0;
			snapshot->freqs[i].max_freq = 0;
			snapshot->freqs[i].max_freq_valid = 0;
			snapshot->freqs[i].boost = -1;
		}
	}

}

static void initialize_idle_snapshot(struct sys_snapshot *snapshot)
{
	/*
	 * snapshot->idle exposes the cpuidle residency view used for LPI columns.
	 * Busy/Idle authority now comes from raw procstat/schedstat counters that
	 * aggregator.c converts into interval percentages, so these arrays remain
	 * display-oriented rather than the sole source of truth for utilization.
	 */
	snapshot->idle = get_idle_states_array();
	snapshot->idle_state_count = get_global_idle_state_count();
	snapshot->authoritative_idle_jiffies = authoritative_idle_jiffies_pool;
	snapshot->authoritative_iowait_jiffies = authoritative_iowait_jiffies_pool;
	snapshot->authoritative_procstat_valid = authoritative_procstat_valid_pool;
	snapshot->authoritative_runtime_ns = authoritative_runtime_ns_pool;
	snapshot->authoritative_runtime_valid = authoritative_runtime_valid_pool;
}

static void collect_idle_snapshot(struct sys_snapshot *snapshot, int tracked,
				  unsigned long long delta_us,
				  unsigned int residency_mask,
				  unsigned int usage_mask)
{
	initialize_idle_snapshot(snapshot);
	(void)tracked;

	/*
	 * Refresh cpuidle state residency for LPI-* reporting, but keep
	 * Busy%/Idle% authoritative in the raw procstat/schedstat counters that
	 * aggregator.c turns into interval percentages.
	 *
	 * This avoids short-interval undercounting when cpuidle stateN/time only
	 * advances on state exit, while still exposing ARM-specific split idle
	 * residency columns.
	 */
	if (is_cpuidle_enabled() &&
	    (residency_mask || usage_mask) &&
	    snapshot->idle && snapshot->idle_state_count > 0)
		update_idle_states(delta_us, residency_mask, usage_mask);
}

static void collect_power_and_temp_snapshot(struct sys_snapshot *snapshot)
{
	int need_power = is_power_enabled() || is_energy_enabled();
	int need_temp = is_temp_enabled();

	/*
	 * Package power and NUMA temperatures are valid platform-level signals on
	 * this machine. Per-CPU power/temp arrays remain capability-gated because
	 * they are not backed by reliable per-core telemetry here.
	 */
	snapshot->package_power_mw = 0;
	snapshot->package_power_valid = 0;
	if (need_power &&
	    read_total_power_mw(&snapshot->package_power_mw) == 0)
		snapshot->package_power_valid = 1;

	/* ---- Per-interval: NUMA temperatures (vdie0, vdie1) ---- */
	if (need_temp) {
		snapshot->numa_temp_count = get_temp_numa_count();
		read_all_numa_temps_checked(snapshot->numa_temps,
					    &snapshot->numa_temp_valid_mask, 16);
	} else {
		snapshot->numa_temp_count = 0;
		snapshot->numa_temp_valid_mask = 0;
		memset(snapshot->numa_temps, 0, sizeof(snapshot->numa_temps));
	}
}

static void collect_system_counter_snapshot(
	struct sys_snapshot *snapshot,
	unsigned int idle_residency_mask)
{
	static unsigned long long procstat_idles[MAX_CPUS];
	static unsigned long long procstat_iowaits[MAX_CPUS];
	static unsigned long long schedstat_runtime[MAX_CPUS];
	static unsigned char procstat_valid[MAX_CPUS];
	static unsigned char schedstat_valid[MAX_CPUS];
	int procstat_limit;
	int schedstat_limit = -1;
	int tracked = get_tracked_cpu_count();
	int need_idle = is_idle_enabled() || is_iowait_enabled() ||
		idle_residency_mask;

	memset(procstat_idles, 0, sizeof(procstat_idles));
	memset(procstat_iowaits, 0, sizeof(procstat_iowaits));
	memset(schedstat_runtime, 0, sizeof(schedstat_runtime));
	memset(procstat_valid, 0, sizeof(procstat_valid));
	memset(schedstat_valid, 0, sizeof(schedstat_valid));
	if (snapshot->authoritative_procstat_valid)
		memset(snapshot->authoritative_procstat_valid, 0,
		       tracked * sizeof(*snapshot->authoritative_procstat_valid));
	if (snapshot->authoritative_runtime_valid)
		memset(snapshot->authoritative_runtime_valid, 0,
		       tracked * sizeof(*snapshot->authoritative_runtime_valid));

	procstat_limit = -1;
	snapshot->counters.sysstat_valid = 0;
	if (need_idle || is_sysstat_enabled()) {
		/* One fresh /proc/stat parse serves idle and system counters. */
		invalidate_proc_stat_cache();
		if (is_sysstat_enabled()) {
			snapshot->counters.sysstat_valid =
				read_ctx_switches(&snapshot->counters.ctx_switches) == 0 &&
				read_interrupts(&snapshot->counters.interrupts) == 0 &&
				read_soft_interrupts(
					&snapshot->counters.soft_interrupts) == 0;
		}
		if (need_idle) {
			procstat_limit =
				read_all_proc_stat_cpu_idle_checked(procstat_idles,
								procstat_iowaits,
								procstat_valid,
								MAX_CPUS);
		}
	}

	for (int i = 0; i < tracked; i++) {
		int cpu_id = get_cpu_id_by_tracked_idx(i);

		if (cpu_id < 0)
			continue;
		/*
		 * Authoritative raw counters are monotonic inputs for interval delta
		 * calculation. Keep the last value in the pool on a transient read
		 * failure, but leave the validity bit clear. The aggregator then emits
		 * an unavailable sample and re-baselines that CPU on recovery instead
		 * of turning missing data into a false 100% busy spike.
		 */
		if (procstat_limit > 0 && cpu_id < procstat_limit &&
		    procstat_valid[cpu_id]) {
			snapshot->authoritative_idle_jiffies[i] = procstat_idles[cpu_id];
			snapshot->authoritative_iowait_jiffies[i] = procstat_iowaits[cpu_id];
			if (snapshot->authoritative_procstat_valid)
				snapshot->authoritative_procstat_valid[i] = 1;
		}

		if (!need_idle || !busy_source_uses_schedstat_cpu(cpu_id))
			continue;

		if (schedstat_limit < 0) {
			schedstat_limit =
				read_all_schedstat_cpu_runtime_checked(schedstat_runtime,
									   schedstat_valid,
									   MAX_CPUS);
		}
		if (schedstat_limit > 0 && cpu_id < schedstat_limit &&
		    schedstat_valid[cpu_id]) {
			snapshot->authoritative_runtime_ns[i] = schedstat_runtime[cpu_id];
			if (snapshot->authoritative_runtime_valid)
				snapshot->authoritative_runtime_valid[i] = 1;
		}
	}

	/* ---- Per-interval: memory bandwidth counter ---- */
	snapshot->counters.mem_bw_counter = 0;
	snapshot->counters.mem_bw_valid = 0;
	if (is_membw_enabled() &&
	    read_mem_bw_raw_checked(&snapshot->counters.mem_bw_counter) == 0)
		snapshot->counters.mem_bw_valid = 1;
}

static void clear_pmu_snapshot(struct sys_snapshot *snapshot)
{
	snapshot->counters.pmu_count = 0;
	snapshot->counters.pmu_per_cpu = NULL;
	snapshot->counters.pmu_per_cpu_valid = NULL;
	snapshot->counters.pmu_valid = 0;
	memset(snapshot->counters.pmu, 0, sizeof(snapshot->counters.pmu));
}

static void collect_pmu_snapshot(struct sys_snapshot *snapshot, int tracked)
{
	unsigned long long total[MAX_PMU_EVENTS] = {0};

	if (!pmu_is_active() || get_pmu_event_count() <= 0) {
		clear_pmu_snapshot(snapshot);
		return;
	}

	snapshot->counters.pmu_count = get_pmu_event_count();
	snapshot->counters.pmu_per_cpu = pmu_pool;
	snapshot->counters.pmu_per_cpu_valid = pmu_valid_pool;
	snapshot->counters.pmu_valid = tracked > 0;
	if (!snapshot->counters.pmu_per_cpu ||
	    !snapshot->counters.pmu_per_cpu_valid) {
		clear_pmu_snapshot(snapshot);
		return;
	}

	read_all_pmu_counters(snapshot->counters.pmu_per_cpu,
			      snapshot->counters.pmu_per_cpu_valid, tracked);

	for (int i = 0; i < tracked; i++) {
		if (!snapshot->counters.pmu_per_cpu_valid[i])
			snapshot->counters.pmu_valid = 0;
		for (int event = 0; event < snapshot->counters.pmu_count; event++) {
			unsigned long long value =
				snapshot->counters.pmu_per_cpu[i][event];

			if (ULLONG_MAX - total[event] < value)
				total[event] = ULLONG_MAX;
			else
				total[event] += value;
		}
	}

	for (int i = 0; i < snapshot->counters.pmu_count; i++)
		snapshot->counters.pmu[i] = total[i];
}

/*
 * collect_sample_data - Fast-path per-interval data collection
 * (internal static version)
 */
static void collect_sample_data(struct sys_snapshot *snapshot)
{
	unsigned int idle_residency_mask = 0;
	unsigned int idle_usage_mask = 0;
	int tracked = get_tracked_cpu_count();

	section_get_idle_collection_masks(&idle_residency_mask,
					  &idle_usage_mask);

	collect_freq_snapshot(snapshot, tracked);
	collect_idle_snapshot(snapshot, tracked, snapshot->interval_delta_us,
			      idle_residency_mask, idle_usage_mask);
	collect_power_and_temp_snapshot(snapshot);
	collect_system_counter_snapshot(snapshot, idle_residency_mask);
	collect_pmu_snapshot(snapshot, tracked);
}

/* === PUBLIC API === */

int init_sample_cache(int cpu_count)
{
	if (pool_init(cpu_count) < 0)
		return -1;
	if (slow_init(cpu_count) < 0) {
		pool_free();
		return -1;
	}
	slow_update_all(cpu_count);
	cache_initialized = 1;
	return 0;
}

/*
 * realloc_sample_cache - Reallocate pools for new CPU count after hotplug
 */
int realloc_sample_cache(int cpu_count)
{
	/*
	 * Hotplug is rare compared to the sampling fast path, so prefer a full
	 * cache rebuild here. This avoids partial realloc state and keeps the
	 * pool/slow-layer lifetimes easy to reason about.
	 */
	free_sample_cache();
	return init_sample_cache(cpu_count);
}

void free_sample_cache(void)
{
	pool_free();
	slow_free();
	cache_initialized = 0;
}

void collect_per_interval_data(struct sys_snapshot *snapshot, unsigned long long delta_us)
{
	/* Collect fast-path data */
	collect_sample_data(snapshot);

	/* Spread slow-changing refreshes across intervals to avoid spikes. */
	maybe_run_slow_refresh(get_tracked_cpu_count(), delta_us);
}

int is_sample_cache_initialized(void)
{
	return cache_initialized;
}
