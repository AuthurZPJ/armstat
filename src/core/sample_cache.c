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
#include "sysfs_util.h"

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
static int diagnostic_counter_collection;

enum freq_collection {
	COLLECT_FREQ_CURRENT = 1U << 0,
	COLLECT_FREQ_UNCORE = 1U << 1,
	COLLECT_FREQ_MIN = 1U << 2,
	COLLECT_FREQ_MAX = 1U << 3,
	COLLECT_FREQ_GOVERNOR = 1U << 4,
	COLLECT_FREQ_BOOST = 1U << 5,
};

#define COLLECT_FREQ_CONFIG (COLLECT_FREQ_MIN | COLLECT_FREQ_MAX | \
			     COLLECT_FREQ_GOVERNOR | COLLECT_FREQ_BOOST)
#define COLLECT_FREQ_ALL (COLLECT_FREQ_CONFIG | COLLECT_FREQ_CURRENT | \
			 COLLECT_FREQ_UNCORE)

static unsigned int freq_collection_mask;
static unsigned int cached_residency_mask;
static unsigned int cached_usage_mask;
static int cached_pmu_event_count = -1;
static int fd_budget_ready;

static unsigned int selected_frequency_fields(void)
{
	unsigned int mask = 0;
	struct field_desc *fields[64];
	int count;

	if (diagnostic_counter_collection)
		return COLLECT_FREQ_ALL;
	for (int scope = FIELD_SCOPE_SYSTEM; scope <= FIELD_SCOPE_CPU; scope++) {
		if ((scope == FIELD_SCOPE_SYSTEM &&
		     !section_is_summary_mode() && !section_emit_default_summary()) ||
		    (scope == FIELD_SCOPE_PACKAGE &&
		     (section_is_summary_mode() || !section_emit_package())) ||
		    (scope == FIELD_SCOPE_CPU &&
		     (section_is_summary_mode() || !section_emit_cpu())))
			continue;
		get_enabled_fields(scope, fields, &count);
		for (int i = 0; i < count; i++) {
			const char *id = fields[i]->id;

			if (!strcmp(id, "freq_mhz") || !strcmp(id, "summary_freq_mhz") ||
			    !strcmp(id, "pkg_freq_mhz"))
				mask |= COLLECT_FREQ_CURRENT;
			else if (!strcmp(id, "uncore_freq"))
				mask |= COLLECT_FREQ_UNCORE;
			else if (!strcmp(id, "min_freq_mhz"))
				mask |= COLLECT_FREQ_MIN;
			else if (!strcmp(id, "max_freq_mhz"))
				mask |= COLLECT_FREQ_MAX;
			else if (!strcmp(id, "governor"))
				mask |= COLLECT_FREQ_GOVERNOR;
			else if (!strcmp(id, "boost"))
				mask |= COLLECT_FREQ_BOOST;
		}
	}
	return mask;
}

static void configure_counter_fd_budget(int tracked, unsigned int residency_mask,
					unsigned int usage_mask)
{
	int state_count = get_global_idle_state_count();
	int current_demand = freq_collection_mask & COLLECT_FREQ_CURRENT ? tracked : 0;
	int idle_demand = 0;
	int pmu_count = pmu_is_active() ? get_pmu_event_count() : 0;
	int total;
	int budget;
	int current_budget;

	if (fd_budget_ready && cached_residency_mask == residency_mask &&
	    cached_usage_mask == usage_mask && cached_pmu_event_count == pmu_count)
		return;
	for (int state = 0; state < state_count; state++) {
		if (residency_mask & (1U << state))
			idle_demand += tracked;
		if (usage_mask & (1U << state))
			idle_demand += tracked;
	}
	total = current_demand + idle_demand;
	/* Existing PMU and inherited fds are counted, with 64 free runtime slots. */
	budget = sysfs_counter_fd_budget(total,
		get_cpufreq_cached_fd_count() + get_cpuidle_cached_fd_count());
	current_budget = total ? (int)((long long)budget * current_demand / total) : 0;
	configure_cpufreq_fd_cache(current_budget, current_demand != 0);
	configure_cpuidle_fd_cache(budget - current_budget, residency_mask, usage_mask);
	cached_residency_mask = residency_mask;
	cached_usage_mask = usage_mask;
	cached_pmu_event_count = pmu_count;
	fd_budget_ready = 1;
}

void set_diagnostic_counter_collection(int enable)
{
	diagnostic_counter_collection = !!enable;
}

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

/* Slow housekeeping uses the same bounded work window for config and idle. */
static int slow_layer_initialized;
static int slow_cursor;
static int slow_full_refresh_done;
static unsigned long long slow_budget_credit;
static unsigned long long slow_sweep;
static unsigned long long slow_sweep_started_us;
static unsigned long long slow_expiry_us;
static unsigned long long slow_cost_ns;
static struct sample_cache_slow_stats slow_stats;

static struct slow_data {
	unsigned int *min_freq;
	unsigned char *min_freq_valid;
	unsigned int *max_freq;
	unsigned char *max_freq_valid;
	int *boost;
	char (*governors)[32];
	int *representatives;
	unsigned long long (*last_success_us)[4];
	unsigned long long *read_sweep;
	unsigned long long *read_generation;
} slow_data;

enum slow_field {
	SLOW_MIN,
	SLOW_MAX,
	SLOW_GOVERNOR,
	SLOW_BOOST,
};

#define SLOW_TARGET_SWEEP_MS_DEFAULT 5000
#define SLOW_MAX_WORK_US 2000
#define SLOW_MIN_WORK_US 50
static int slow_target_sweep_ms = SLOW_TARGET_SWEEP_MS_DEFAULT;

static unsigned long long slow_now_us(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (unsigned long long)now.tv_sec * 1000000ULL + now.tv_nsec / 1000;
}

static unsigned long long slow_age_us(unsigned long long now,
				     unsigned long long then)
{
	return now >= then ? now - then : 0;
}

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
	slow_data.representatives = calloc(alloc_count, sizeof(int));
	slow_data.last_success_us = calloc(alloc_count,
					 sizeof(*slow_data.last_success_us));
	slow_data.read_sweep = calloc(alloc_count, sizeof(unsigned long long));
	slow_data.read_generation = calloc(alloc_count, sizeof(unsigned long long));
	if (!slow_data.min_freq || !slow_data.min_freq_valid ||
	    !slow_data.max_freq || !slow_data.max_freq_valid ||
	    !slow_data.boost || !slow_data.governors ||
	    !slow_data.representatives || !slow_data.last_success_us ||
	    !slow_data.read_sweep || !slow_data.read_generation) {
		slow_free();
		return -1;
	}

	for (int i = 0; i < count; i++) {
		slow_data.boost[i] = -1;
		slow_data.representatives[i] = i;
	}
	slow_cursor = 0;
	slow_full_refresh_done = 0;
	slow_budget_credit = 0;
	slow_sweep = 0;
	slow_cost_ns = 0;
	memset(&slow_stats, 0, sizeof(slow_stats));

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
	slow_expiry_us = (unsigned long long)slow_target_sweep_ms * 2000;
	slow_layer_initialized = 1;
	return 0;
}

static void slow_free(void)
{
	free(slow_data.min_freq);
	free(slow_data.min_freq_valid);
	free(slow_data.max_freq);
	free(slow_data.max_freq_valid);
	free(slow_data.boost);
	free(slow_data.governors);
	free(slow_data.representatives);
	free(slow_data.last_success_us);
	free(slow_data.read_sweep);
	free(slow_data.read_generation);
	memset(&slow_data, 0, sizeof(slow_data));
	memset(&slow_stats, 0, sizeof(slow_stats));
	slow_cursor = 0;
	slow_full_refresh_done = 0;
	slow_budget_credit = 0;
	slow_sweep = 0;
	slow_cost_ns = 0;
	slow_layer_initialized = 0;
}

static int slow_budget_for_interval(int tracked, unsigned long long delta_us)
{
	unsigned long long target_us = (unsigned long long)slow_target_sweep_ms * 1000ULL;

	if (tracked <= 0 || delta_us == 0)
		return 0;
	if (delta_us > target_us)
		delta_us = target_us;
	slow_budget_credit += (unsigned long long)tracked * delta_us;
	if (slow_budget_credit > (unsigned long long)tracked * target_us)
		slow_budget_credit = (unsigned long long)tracked * target_us;
	return (int)(slow_budget_credit / target_us);
}

static void slow_begin_sweep(void)
{
	if (++slow_sweep == 0) {
		slow_sweep = 1;
		memset(slow_data.read_sweep, 0,
		       get_tracked_cpu_count() * sizeof(*slow_data.read_sweep));
	}
	if (freq_collection_mask & COLLECT_FREQ_CONFIG)
		cpufreq_begin_config_sweep();
	slow_sweep_started_us = slow_now_us();
}

static void slow_refresh_cpu(int tracked_idx)
{
	unsigned int min_freq;
	unsigned int max_freq;
	unsigned long long generation;
	unsigned long long now;
	int min_valid;
	int max_valid;
	int representative;

	if (!(freq_collection_mask & COLLECT_FREQ_CONFIG) || tracked_idx < 0 ||
	    tracked_idx >= get_tracked_cpu_count())
		return;

	representative = get_cpufreq_config_representative(tracked_idx);
	if (representative < 0 || representative >= get_tracked_cpu_count())
		representative = tracked_idx;
	slow_data.representatives[tracked_idx] = representative;
	generation = get_cpufreq_config_generation(representative);
	if (slow_data.read_sweep[representative] == slow_sweep &&
	    slow_data.read_generation[representative] == generation)
		return;
	slow_data.read_sweep[representative] = slow_sweep;
	slow_data.read_generation[representative] = generation;
	slow_stats.config_reads++;

	read_cpu_min_max_freq_selected(representative, &min_freq, &min_valid,
				     &max_freq, &max_valid,
				     freq_collection_mask & COLLECT_FREQ_MIN,
				     freq_collection_mask & COLLECT_FREQ_MAX);
	now = slow_now_us();
	slow_data.min_freq[representative] = min_freq;
	slow_data.min_freq_valid[representative] = min_valid;
	slow_data.max_freq[representative] = max_freq;
	slow_data.max_freq_valid[representative] = max_valid;
	if (min_valid)
		slow_data.last_success_us[representative][SLOW_MIN] = now;
	if (max_valid)
		slow_data.last_success_us[representative][SLOW_MAX] = now;
	if (freq_collection_mask & COLLECT_FREQ_BOOST) {
		int boost = -1;

		if (read_cpu_boost(representative, &boost) == 0)
			slow_data.last_success_us[representative][SLOW_BOOST] = slow_now_us();
		slow_data.boost[representative] = boost;
	}
	if (freq_collection_mask & COLLECT_FREQ_GOVERNOR) {
		slow_data.governors[representative][0] = '\0';
		if (read_cpu_governor(representative,
				      slow_data.governors[representative], 32) == 0)
			slow_data.last_success_us[representative][SLOW_GOVERNOR] = slow_now_us();
	}
}

static void slow_update_all(int tracked)
{
	slow_begin_sweep();
	for (int i = 0; i < tracked; i++)
		slow_refresh_cpu(i);
	slow_cursor = 0;
	slow_full_refresh_done = 1;
	/* Startup completes before measured sampling; start the timed sweep now. */
	slow_begin_sweep();
}

static void maybe_run_slow_refresh(int tracked, unsigned long long delta_us)
{
	unsigned long long target_us = (unsigned long long)slow_target_sweep_ms * 1000ULL;
	unsigned long long started;
	unsigned long long budget_us;
	int budget;
	int idle_enabled;

	slow_stats.work_us = 0;
	slow_stats.refreshed_cpus = 0;
	slow_stats.config_reads = 0;
	slow_stats.time_budget_us = 0;
	if (!slow_layer_initialized || tracked <= 0)
		return;
	budget = slow_budget_for_interval(tracked, delta_us);
	idle_enabled = is_cpuidle_enabled() && (cached_residency_mask || cached_usage_mask);
	if (budget <= 0 || (!(freq_collection_mask & COLLECT_FREQ_CONFIG) && !idle_enabled))
		return;

	/* Spend at most 1% of the interval, bounded to 50 us .. 2 ms. */
	budget_us = delta_us / 100;
	if (budget_us < SLOW_MIN_WORK_US)
		budget_us = SLOW_MIN_WORK_US;
	if (budget_us > SLOW_MAX_WORK_US)
		budget_us = SLOW_MAX_WORK_US;
	slow_stats.time_budget_us = budget_us;
	started = slow_now_us();
	for (int refreshed = 0; refreshed < budget; refreshed++) {
		unsigned long long before = slow_now_us();
		unsigned long long elapsed = slow_age_us(before, started);
		unsigned long long cost_ns;

		if (refreshed && (elapsed >= budget_us ||
		    slow_cost_ns / 1000 > budget_us - elapsed))
			break;
		slow_refresh_cpu(slow_cursor);
		if (idle_enabled)
			refresh_idle_state_disable_cache_budgeted(1);
		slow_stats.refreshed_cpus++;
		slow_budget_credit -= target_us;
		if (++slow_cursor == tracked) {
			slow_cursor = 0;
			slow_stats.last_sweep_us = slow_age_us(slow_now_us(), slow_sweep_started_us);
			slow_begin_sweep();
		}
		cost_ns = slow_age_us(slow_now_us(), before) * 1000;
		slow_cost_ns = slow_cost_ns ? (slow_cost_ns * 7 + cost_ns) / 8 : cost_ns;
	}
	slow_stats.work_us = slow_age_us(slow_now_us(), started);
}

static int slow_field_fresh(int representative, int field, unsigned long long now)
{
	unsigned long long success = slow_data.last_success_us[representative][field];

	return success && slow_age_us(now, success) <= slow_expiry_us;
}

static int slow_source_matches(int tracked_idx, int representative)
{
	return cpufreq_config_group_matches(tracked_idx, representative) &&
		slow_data.read_generation[representative] ==
		get_cpufreq_config_generation(representative);
}

void get_sample_cache_slow_stats(struct sample_cache_slow_stats *stats)
{
	unsigned long long now = (freq_collection_mask & COLLECT_FREQ_CONFIG) ?
		slow_now_us() : 0;
	const unsigned int masks[] = {
		COLLECT_FREQ_MIN, COLLECT_FREQ_MAX, COLLECT_FREQ_GOVERNOR, COLLECT_FREQ_BOOST,
	};

	if (!stats)
		return;
	*stats = slow_stats;
	stats->max_age_us = 0;
	stats->stale_cpus = 0;
	stats->unavailable_cpus = 0;
	if (!(freq_collection_mask & COLLECT_FREQ_CONFIG))
		return;
	for (int i = 0; slow_layer_initialized && i < get_tracked_cpu_count(); i++) {
		int representative = slow_data.representatives[i];
		int stale = 0;
		int unavailable = !slow_source_matches(i, representative);

		for (int field = SLOW_MIN; field <= SLOW_BOOST; field++) {
			unsigned long long success;
			unsigned long long age;
			int valid;

			if (!(freq_collection_mask & masks[field]))
				continue;
			success = slow_data.last_success_us[representative][field];
			age = success ? slow_age_us(now, success) : 0;
			if (age > stats->max_age_us)
				stats->max_age_us = age;
			stale |= success && age > slow_expiry_us;
			valid = field == SLOW_MIN ? slow_data.min_freq_valid[representative] :
				field == SLOW_MAX ? slow_data.max_freq_valid[representative] :
				field == SLOW_GOVERNOR ? !!slow_data.governors[representative][0] :
				slow_data.boost[representative] >= 0;
			unavailable |= !valid || !slow_field_fresh(representative, field, now);
		}
		stats->stale_cpus += stale;
		stats->unavailable_cpus += unavailable;
	}
}

/*
 * collect_sample_data - Fast-path per-interval data collection
 * (internal static version)
 */
static void collect_freq_snapshot(struct sys_snapshot *snapshot, int tracked)
{
	unsigned long long now = (freq_collection_mask & COLLECT_FREQ_CONFIG) ?
		slow_now_us() : 0;

	snapshot->freqs = (freq_collection_mask &
		(COLLECT_FREQ_CURRENT | COLLECT_FREQ_CONFIG)) ? freqs_pool : NULL;
	snapshot->uncore_freq_hz = 0;
	snapshot->uncore_freq_valid = 0;
	if (!freq_collection_mask)
		return;

	/*
	 * Uncore/devfreq is a platform-level summary signal. Keep its sampling
	 * independent from the per-CPU cpufreq array so the code reads as
	 * "summary frequency + per-CPU frequencies", not as one monolithic block.
	 */
	if ((freq_collection_mask & COLLECT_FREQ_UNCORE) &&
	    read_uncore_freq(&snapshot->uncore_freq_hz) == 0)
		snapshot->uncore_freq_valid = 1;

	if (!snapshot->freqs)
		return;

	for (int i = 0; i < tracked; i++) {
		snapshot->freqs[i].cur_freq_valid =
			(freq_collection_mask & COLLECT_FREQ_CURRENT) &&
			read_cpu_freq(i, &snapshot->freqs[i].cur_freq) == 0;
		if (!snapshot->freqs[i].cur_freq_valid)
			snapshot->freqs[i].cur_freq = 0;
		if (slow_layer_initialized && slow_data.min_freq &&
		    (freq_collection_mask & COLLECT_FREQ_CONFIG)) {
			int representative = slow_data.representatives[i];
			int matches = slow_source_matches(i, representative);

			snapshot->freqs[i].min_freq = slow_data.min_freq[representative];
			snapshot->freqs[i].min_freq_valid = matches &&
				slow_data.min_freq_valid[representative] &&
				slow_field_fresh(representative, SLOW_MIN, now);
			snapshot->freqs[i].max_freq = slow_data.max_freq[representative];
			snapshot->freqs[i].max_freq_valid = matches &&
				slow_data.max_freq_valid[representative] &&
				slow_field_fresh(representative, SLOW_MAX, now);
			snapshot->freqs[i].boost = matches &&
				slow_field_fresh(representative, SLOW_BOOST, now) ?
				slow_data.boost[representative] : -1;
			copy_cstring(snapshot->freqs[i].governor,
				     sizeof(snapshot->freqs[i].governor), matches &&
				     slow_field_fresh(representative, SLOW_GOVERNOR, now) ?
				     slow_data.governors[representative] : "");
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

static void collect_system_counter_snapshot(struct sys_snapshot *snapshot)
{
	static unsigned long long procstat_idles[MAX_CPUS];
	static unsigned long long procstat_iowaits[MAX_CPUS];
	static unsigned long long schedstat_runtime[MAX_CPUS];
	static unsigned char procstat_valid[MAX_CPUS];
	static unsigned char schedstat_valid[MAX_CPUS];
	int procstat_limit;
	int schedstat_limit = -1;
	int schedstat_attempted = 0;
	int tracked = get_tracked_cpu_count();
	int need_idle = is_idle_enabled() || is_iowait_enabled();

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

		if (!diagnostic_counter_collection &&
		    (!need_idle || !busy_source_uses_schedstat_cpu(cpu_id)))
			continue;

		if (!schedstat_attempted) {
			schedstat_attempted = 1;
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
	unsigned int selected_freq = selected_frequency_fields();

	if (selected_freq != freq_collection_mask) {
		freq_collection_mask = selected_freq;
		fd_budget_ready = 0;
		slow_full_refresh_done = 0;
		slow_budget_credit = 0;
	}

	section_get_idle_collection_masks(&idle_residency_mask,
					  &idle_usage_mask);
	if (diagnostic_counter_collection) {
		idle_residency_mask = (1U << MAX_IDLE_STATES) - 1U;
		idle_usage_mask = idle_residency_mask;
	}
	configure_counter_fd_budget(tracked, idle_residency_mask, idle_usage_mask);
	if (!slow_full_refresh_done)
		slow_update_all(tracked);

	collect_freq_snapshot(snapshot, tracked);
	collect_idle_snapshot(snapshot, tracked, snapshot->interval_delta_us,
			      idle_residency_mask, idle_usage_mask);
	collect_power_and_temp_snapshot(snapshot);
	collect_system_counter_snapshot(snapshot);
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
	freq_collection_mask = selected_frequency_fields();
	fd_budget_ready = 0;
	sysfs_counter_begin_sample();
	cpufreq_begin_sample();
	slow_update_all(cpu_count);
	sysfs_counter_end_sample();
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
	fd_budget_ready = 0;
	freq_collection_mask = 0;
}

void collect_per_interval_data(struct sys_snapshot *snapshot, unsigned long long delta_us)
{
	slow_expiry_us = (unsigned long long)slow_target_sweep_ms * 2000ULL;
	if (delta_us > slow_expiry_us / 2)
		slow_expiry_us = delta_us <= ULLONG_MAX / 2 ? delta_us * 2 : ULLONG_MAX;
	sysfs_counter_begin_sample();
	cpufreq_begin_sample();
	/* Collect fast-path data */
	collect_sample_data(snapshot);

	/* Spread slow-changing refreshes across intervals to avoid spikes. */
	maybe_run_slow_refresh(get_tracked_cpu_count(), delta_us);
	sysfs_counter_end_sample();
}

int is_sample_cache_initialized(void)
{
	return cache_initialized;
}
