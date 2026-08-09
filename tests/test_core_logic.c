// SPDX-License-Identifier: GPL-2.0
/*
 * Unit tests for core calculation logic: aggregator idle%, LPI residual.
 *
 * These tests use synthetic sys_snapshot data so they run without real
 * hardware. They validate the formulas, not the data sources.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../collector.h"
#include "../aggregator.h"
#include "../formatter.h"
#include "../cpu_inventory.h"
#include "../idle_backend.h"
#include "../sysstat.h"

/* Minimal seed for a single-CPU catalog so tracked_idx=0 → cpu_id=0 */
static void seed_one_cpu(void)
{
	struct cpu_inventory_seed cpus[1] = {
		{0, 1, 1, 0, 0, 0},
	};

	cpu_inventory_seed(cpus, 1);
}

static void seed_two_cpus_sparse_packages(void)
{
	struct cpu_inventory_seed cpus[2] = {
		{0, 1, 1, 2, 0, 0},
		{1, 1, 1, 4, 0, 0},
	};

	cpu_inventory_seed(cpus, 2);
}

static double jiffies_percent(unsigned long long jiffies,
			      unsigned long long delta_us)
{
	unsigned long long delta_jiffies_us;

	delta_jiffies_us = (jiffies * 1000000ULL) / get_kernel_hz();
	return (double)delta_jiffies_us * 100.0 / (double)delta_us;
}

/* ============================================================================
 * Aggregator idle% tests
 * ============================================================================ */

static void test_aggregator_idle_100_pct(void)
{
	struct sys_snapshot snap;
	struct interval_stats stats;

	seed_one_cpu();
	set_busy_source_mode(BUSY_SOURCE_PROCSTAT);
	init_aggregator();

	memset(&snap, 0, sizeof(snap));
	snap.cpu_count          = 1;
	snap.effective_cpu_count = 1;
	snap.interval_delta_us   = 1000000ULL; /* 1 second */

	/* Authoritative idle baseline. */
	unsigned long long idle_jiffies  = (unsigned long long)get_kernel_hz();
	unsigned long long iowait_jiffies = 0;
	unsigned long long runtime_ns    = 0;

	snap.authoritative_idle_jiffies  = &idle_jiffies;
	snap.authoritative_iowait_jiffies = &iowait_jiffies;
	snap.authoritative_runtime_ns    = &runtime_ns;

	/* First call sets baseline, second calculates delta */
	calculate_interval_stats(&snap, &stats);

	idle_jiffies += (unsigned long long)get_kernel_hz();
	calculate_interval_stats(&snap, &stats);

	/* 1s idle out of 1s interval = 100% idle */
	assert(fabs(stats.per_cpu_idle[0] - 100.0) < 0.01);
	assert(fabs(stats.per_cpu_busy[0] - 0.0)   < 0.01);

	cleanup_aggregator();
}

static void test_aggregator_busy_50_pct(void)
{
	struct sys_snapshot snap;
	struct interval_stats stats;

	seed_one_cpu();
	set_busy_source_mode(BUSY_SOURCE_PROCSTAT);
	init_aggregator();

	memset(&snap, 0, sizeof(snap));
	snap.cpu_count          = 1;
	snap.effective_cpu_count = 1;
	snap.interval_delta_us   = 1000000ULL;

	unsigned long long idle_jiffies   = (unsigned long long)get_kernel_hz();
	unsigned long long iowait_jiffies = 0;
	unsigned long long runtime_ns     = 0;
	unsigned long long idle_delta;
	double expected_idle;

	snap.authoritative_idle_jiffies   = &idle_jiffies;
	snap.authoritative_iowait_jiffies = &iowait_jiffies;
	snap.authoritative_runtime_ns     = &runtime_ns;

	calculate_interval_stats(&snap, &stats);
	idle_delta = (unsigned long long)get_kernel_hz() / 2ULL;
	idle_jiffies += idle_delta;
	calculate_interval_stats(&snap, &stats);

	expected_idle = jiffies_percent(idle_delta, snap.interval_delta_us);
	assert(fabs(stats.per_cpu_idle[0] - expected_idle) < 0.01);
	assert(fabs(stats.per_cpu_busy[0] - (100.0 - expected_idle)) < 0.01);

	cleanup_aggregator();
}

static void test_aggregator_iowait_independent(void)
{
	struct sys_snapshot snap;
	struct interval_stats stats;

	seed_one_cpu();
	set_busy_source_mode(BUSY_SOURCE_PROCSTAT);
	init_aggregator();

	memset(&snap, 0, sizeof(snap));
	snap.cpu_count          = 1;
	snap.effective_cpu_count = 1;
	snap.interval_delta_us   = 1000000ULL;

	unsigned long long idle_jiffies   = (unsigned long long)get_kernel_hz();
	unsigned long long iowait_jiffies = 0;
	unsigned long long runtime_ns     = 0;
	unsigned long long idle_delta;
	unsigned long long iowait_delta;
	double expected_idle;
	double expected_iowait;

	snap.authoritative_idle_jiffies   = &idle_jiffies;
	snap.authoritative_iowait_jiffies = &iowait_jiffies;
	snap.authoritative_runtime_ns     = &runtime_ns;

	calculate_interval_stats(&snap, &stats);

	/* iowait is a subset of idle. */
	idle_delta = (unsigned long long)get_kernel_hz() / 5ULL;
	iowait_delta = (unsigned long long)get_kernel_hz() / 10ULL;
	idle_jiffies += idle_delta;
	iowait_jiffies += iowait_delta;
	calculate_interval_stats(&snap, &stats);

	expected_idle = jiffies_percent(idle_delta, snap.interval_delta_us);
	expected_iowait = jiffies_percent(iowait_delta, snap.interval_delta_us);
	assert(fabs(stats.per_cpu_idle[0] - expected_idle) < 0.01);
	assert(fabs(stats.per_cpu_busy[0] - (100.0 - expected_idle)) < 0.01);
	assert(fabs(stats.per_cpu_iowait[0] - expected_iowait) < 0.01);

	cleanup_aggregator();
}

static void test_aggregator_schedstat_clamp(void)
{
	/* Verify that schedstat runtime > wall clock is clamped */
	struct sys_snapshot snap;
	struct interval_stats stats;

	seed_one_cpu();
	set_busy_source_mode(BUSY_SOURCE_SCHEDSTAT);
	init_aggregator();

	memset(&snap, 0, sizeof(snap));
	snap.cpu_count          = 1;
	snap.effective_cpu_count = 1;
	snap.interval_delta_us   = 1000000ULL; /* 1 second */

	unsigned long long idle_jiffies   = 0;
	unsigned long long iowait_jiffies = 0;
	unsigned long long runtime_ns     = 0;
	unsigned char runtime_valid       = 1;

	snap.authoritative_idle_jiffies   = &idle_jiffies;
	snap.authoritative_iowait_jiffies = &iowait_jiffies;
	snap.authoritative_runtime_ns     = &runtime_ns;
	snap.authoritative_runtime_valid  = &runtime_valid;

	calculate_interval_stats(&snap, &stats);

	/* runtime_ns = 2 seconds (exceeds wall clock) — should be clamped */
	runtime_ns = 2000000000ULL;
	calculate_interval_stats(&snap, &stats);

	assert(fabs(stats.per_cpu_idle[0] - 0.0)   < 0.01);
	assert(fabs(stats.per_cpu_busy[0] - 100.0) < 0.01);

	cleanup_aggregator();
}

static void test_aggregator_sparse_package_ids(void)
{
	struct sys_snapshot snap;
	struct interval_stats stats;
	unsigned long long idle_jiffies[2] = {0, 0};
	unsigned long long iowait_jiffies[2] = {0, 0};
	unsigned long long runtime_ns[2] = {0, 0};

	seed_two_cpus_sparse_packages();
	set_busy_source_mode(BUSY_SOURCE_PROCSTAT);
	init_aggregator();

	memset(&snap, 0, sizeof(snap));
	snap.cpu_count = 2;
	snap.effective_cpu_count = 2;
	snap.interval_delta_us = 1000000ULL;
	snap.authoritative_idle_jiffies = idle_jiffies;
	snap.authoritative_iowait_jiffies = iowait_jiffies;
	snap.authoritative_runtime_ns = runtime_ns;

	calculate_interval_stats(&snap, &stats);

	assert(stats.package_count == 2);
	assert(stats.packages[0].package_id == 2);
	assert(stats.packages[0].cpu_count == 1);
	assert(stats.packages[1].package_id == 4);
	assert(stats.packages[1].cpu_count == 1);

	cleanup_aggregator();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
	test_aggregator_idle_100_pct();
	test_aggregator_busy_50_pct();
	test_aggregator_iowait_independent();
	test_aggregator_schedstat_clamp();
	test_aggregator_sparse_package_ids();

	printf("test_core_logic: all tests passed\n");
	return 0;
}
