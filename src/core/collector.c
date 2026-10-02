/* SPDX-License-Identifier: GPL-2.0 */
/*
 * collector.c - Data collection orchestrator
 *
 * This is now a thin orchestrator that delegates to specialized modules:
 *   - cpu_inventory: CPU ID mapping and hotplug detection
 *   - sample_cache: Memory pools and fast-path sampling
 *   - authoritative idle raw counters: captured in sample_cache and turned
 *     into percentages in aggregator.c alongside the other interval deltas
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "collector.h"
#include "cpufreq.h"
#include "cpuidle.h"
#include "power.h"
#include "pmu.h"
#include "topology.h"
#include "cpu_inventory.h"
#include "sample_cache.h"
#include "sampling_deadline.h"
#include "aggregator.h"
#include "formatter.h"
#include "sysstat.h"

static unsigned long long prev_collector_time_us;

struct collector_time {
	unsigned long long monotonic_us;
	unsigned long long monotonic_ns;
	time_t realtime_seconds;
	unsigned long long realtime_ns;
};

static int capture_collector_time(struct collector_time *sample_time)
{
	struct timespec monotonic;
	struct timespec realtime;
	time_t fallback;

	if (!sample_time)
		return -1;

	if (clock_gettime(CLOCK_MONOTONIC, &monotonic) < 0) {
		fprintf(stderr, "Error: clock_gettime(CLOCK_MONOTONIC) failed: %s\n",
			strerror(errno));
		return -1;
	}
	sample_time->monotonic_us =
		(unsigned long long)monotonic.tv_sec * 1000000ULL +
		(unsigned long long)monotonic.tv_nsec / 1000ULL;
	sample_time->monotonic_ns =
		(unsigned long long)monotonic.tv_sec * 1000000000ULL +
		(unsigned long long)monotonic.tv_nsec;

	if (clock_gettime(CLOCK_REALTIME, &realtime) == 0) {
		sample_time->realtime_seconds = realtime.tv_sec;
		sample_time->realtime_ns =
			(unsigned long long)realtime.tv_sec * 1000000000ULL +
			(unsigned long long)realtime.tv_nsec;
		return 0;
	}

	fallback = time(NULL);
	if (fallback == (time_t)-1) {
		fprintf(stderr, "Error: time() failed while reading realtime clock\n");
		return -1;
	}
	sample_time->realtime_seconds = fallback;
	sample_time->realtime_ns =
		(unsigned long long)fallback * 1000000000ULL;
	return 0;
}

static void disable_cpuidle_with_warning(const char *reason)
{
	if (!is_cpuidle_enabled())
		return;

	close_cpuidle();

	if (reason && *reason)
		fprintf(stderr, "Warning: %s, disabling cpuidle split LPI reporting\n",
			reason);
	enable_cpuidle(0);
}

static void sync_cpuidle_runtime_state(int hotplug_rebuild)
{
	/*
	 * cpuidle has its own runtime caches and file descriptors. Keep its
	 * lifecycle tied to the collector so split-LPI reporting is either fully
	 * available or explicitly disabled, never half-initialized.
	 */
	if (!is_cpuidle_enabled())
		return;

	if (hotplug_rebuild)
		close_cpuidle();

	if (init_cpuidle() < 0)
		disable_cpuidle_with_warning("cpuidle init failed");

	/*
	 * LPI column visibility is derived from the live cpuidle runtime state.
	 * Keep the formatter in sync so a failed re-init hides split-idle fields
	 * instead of leaving stale LPI headers behind.
	 */
	update_idle_state_visibility();
}

int rebuild_hotplug_dependent_state(void)
{
	int tracked = get_tracked_cpu_count();

	set_effective_cpu_count(tracked);

	/*
	 * cpufreq caches descriptors in tracked-index order. Rebuild it before the
	 * sample cache so newly added CPUs and reordered sparse CPU IDs cannot use
	 * stale descriptors from the previous tracked set.
	 */
	close_cpufreq();
	if (init_cpufreq() < 0) {
		fprintf(stderr, "Error: failed to rebuild cpufreq state after CPU topology change\n");
		return -1;
	}

	if (realloc_sample_cache(tracked) < 0) {
		fprintf(stderr, "Error: failed to rebuild sample cache after CPU topology change\n");
		close_cpufreq();
		return -1;
	}

	sync_cpuidle_runtime_state(1);
	if (get_pmu_event_count() > 0 && rebuild_pmu_events() < 0)
		fprintf(stderr, "Warning: PMU rebuild failed after CPU topology change; PMU columns will be unavailable\n");

	/*
	 * topology is derived from cpu_catalog and must be refreshed whenever
	 * the tracked CPU set changes. Reset aggregator state so the current
	 * sample becomes a new baseline rather than mixing pre/post-hotplug
	 * counters in one interval.
	 */
	close_topology();
	if (init_topology() < 0)
		fprintf(stderr, "Warning: failed to refresh topology after CPU topology change\n");
	reset_aggregator();
	return 0;
}

/* === PUBLIC API === */

int init_collector(void)
{
	prev_collector_time_us = 0;

	/* Initialize CPU inventory first — cpufreq and other subsystems
	 * depend on CPU-ID array sizing from the inventory. */
	if (init_cpu_inventory() < 0)
		return -1;

	/* Initialize subsystems */
	if (init_cpufreq() < 0) {
		cleanup_cpu_inventory();
		return -1;
	}
	if (init_power() < 0) {
		close_cpufreq();
		cleanup_cpu_inventory();
		return -1;
	}

	/* Get tracked CPU count */
	int tracked = get_tracked_cpu_count();
	set_effective_cpu_count(tracked);

	/* Initialize sample cache (pools + slow layer) */
	if (init_sample_cache(tracked) < 0) {
		cleanup_cpu_inventory();
		close_power();
		close_cpufreq();
		return -1;
	}

	sync_cpuidle_runtime_state(0);

	return 0;
}

int collect_snapshot(struct sys_snapshot *snapshot)
{
	struct collector_time sample_time;
	unsigned long long delta_us;

	if (!snapshot)
		return -1;

	/* Get current time and calculate delta. */
	if (capture_collector_time(&sample_time) < 0) {
		memset(snapshot, 0, sizeof(*snapshot));
		return -1;
	}
	delta_us = sampling_interval_delta_us(prev_collector_time_us,
					       sample_time.monotonic_us, 0);

	/*
	 * Skip hotplug detection for the baseline sample. Until we have a valid
	 * previous interval, a rebuild here only resets freshly initialized state
	 * and can amplify transient inventory noise into user-visible warnings.
	 */
	if (delta_us > 0 && check_and_rebuild_inventory()) {
		if (rebuild_hotplug_dependent_state() < 0) {
			memset(snapshot, 0, sizeof(*snapshot));
			snapshot->cpu_count = cpu_catalog_online_count();
			snapshot->effective_cpu_count = cpu_catalog_tracked_count();
			snapshot->cpu_truncated =
				(!cpu_inventory_filter_is_active() &&
				 snapshot->effective_cpu_count < snapshot->cpu_count) ? 1 : 0;
			prev_collector_time_us = 0;
			return -1;
		}
		/*
		 * A rebuilt runtime state has no meaningful "previous sample".
		 * Re-capture both clocks after rebuild work, then force the current
		 * collection to become the new baseline. This keeps rebuild latency
		 * out of the next visible interval's rate denominator and prevents
		 * pre/post-hotplug counters from being mixed.
		 */
		if (capture_collector_time(&sample_time) < 0) {
			memset(snapshot, 0, sizeof(*snapshot));
			prev_collector_time_us = 0;
			return -1;
		}
		delta_us = sampling_interval_delta_us(prev_collector_time_us,
						       sample_time.monotonic_us, 1);
	}
	prev_collector_time_us = sample_time.monotonic_us;

	/* Clear snapshot */
	memset(snapshot, 0, sizeof(*snapshot));

	/* Set CPU counts from catalog */
	snapshot->cpu_count = cpu_catalog_online_count();
	snapshot->effective_cpu_count = cpu_catalog_tracked_count();
	snapshot->cpu_truncated =
		(!cpu_inventory_filter_is_active() &&
		 snapshot->effective_cpu_count < snapshot->cpu_count) ? 1 : 0;

	/* Store unified time delta */
	snapshot->interval_delta_us = delta_us;
	snapshot->sample_monotonic_ns = sample_time.monotonic_ns;
	snapshot->sample_timestamp = sample_time.realtime_seconds;
	snapshot->sample_timestamp_ns = sample_time.realtime_ns;

	/*
	 * Sampling pools may have been torn down by a failed hotplug rebuild.
	 * Do not continue into fast-path collection with NULL pool pointers.
	 */
	if (!is_sample_cache_initialized()) {
		snapshot->interval_delta_us = 0;
		return -1;
	}

	/* Delegate to sample_cache for fast-path data collection */
	collect_per_interval_data(snapshot, delta_us);
	return 0;
}

void cleanup_collector(void)
{
	/* Cleanup modules */
	free_sample_cache();

	close_cpuidle();

	/* Cleanup subsystems in reverse init order */
	close_power();
	close_cpufreq();
	cleanup_cpu_inventory();
	prev_collector_time_us = 0;
}

/* ============================================================================
 * SNAPSHOT ACCESSORS
 * ============================================================================ */

int sys_snapshot_get_effective_cpu_count(const struct sys_snapshot *s)
{
	return s ? s->effective_cpu_count : 0;
}

int sys_snapshot_get_cpu_truncated(const struct sys_snapshot *s)
{
	return s ? s->cpu_truncated : 0;
}

unsigned long long sys_snapshot_get_interval_delta_us(const struct sys_snapshot *s)
{
	return s ? s->interval_delta_us : 0;
}

const struct cpu_freq_info *sys_snapshot_get_freqs(const struct sys_snapshot *s)
{
	return s ? s->freqs : NULL;
}

struct raw_counters sys_snapshot_get_counters(const struct sys_snapshot *s)
{
	if (s)
		return s->counters;

	struct raw_counters zero = {0};
	return zero;
}
