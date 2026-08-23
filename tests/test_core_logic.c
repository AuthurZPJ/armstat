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
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "collector.h"
#include "aggregator.h"
#include "formatter.h"
#include "cpu_inventory.h"
#include "idle_backend.h"
#include "cpuidle.h"
#include "power.h"
#include "pmu.h"
#include "sysstat.h"
#include "sysfs_util.h"
#include "idle_display.h"
#include "sampling_deadline.h"

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

static void replace_test_file_contents(int fd, const char *text)
{
	size_t len = strlen(text);

	assert(ftruncate(fd, 0) == 0);
	assert(lseek(fd, 0, SEEK_SET) == 0);
	assert(write(fd, text, len) == (ssize_t)len);
}

static void test_checked_numeric_readers_reject_malformed_values(void)
{
	char path[] = "/tmp/armstat-numeric.XXXXXX";
	char oversized[200];
	unsigned long long ull;
	int value;
	int fd = mkstemp(path);

	assert(fd >= 0);
	replace_test_file_contents(fd, "42\n");
	assert(sysfs_read_int_checked(path, &value) == 0 && value == 42);
	assert(sysfs_read_ull_checked(path, &ull) == 0 && ull == 42);
	assert(fd_read_ull_checked(fd, &ull) == 0 && ull == 42);

	replace_test_file_contents(fd, "-1\n");
	assert(sysfs_read_int_checked(path, &value) == 0 && value == -1);
	assert(fd_read_int_checked(fd, &value) == 0 && value == -1);
	assert(sysfs_read_ull_checked(path, &ull) < 0 && ull == 0);
	assert(fd_read_ull_checked(fd, &ull) < 0 && ull == 0);

	replace_test_file_contents(fd, "12garbage\n");
	assert(sysfs_read_int_checked(path, &value) < 0 && value == 0);
	assert(sysfs_read_ull_checked(path, &ull) < 0 && ull == 0);
	assert(fd_read_ull_checked(fd, &ull) < 0 && ull == 0);

	replace_test_file_contents(fd, "18446744073709551616\n");
	assert(sysfs_read_ull_checked(path, &ull) < 0 && ull == 0);
	assert(fd_read_ull_checked(fd, &ull) < 0 && ull == 0);

	memset(oversized, ' ', sizeof(oversized));
	oversized[0] = '4';
	oversized[1] = '2';
	oversized[sizeof(oversized) - 2] = 'x';
	oversized[sizeof(oversized) - 1] = '\0';
	replace_test_file_contents(fd, oversized);
	assert(sysfs_read_int_checked(path, &value) < 0 && value == 0);
	assert(sysfs_read_ull_checked(path, &ull) < 0 && ull == 0);
	assert(fd_read_int_checked(fd, &value) < 0 && value == 0);
	assert(fd_read_ull_checked(fd, &ull) < 0 && ull == 0);

	replace_test_file_contents(fd, "2147483648\n");
	assert(sysfs_read_int_checked(path, &value) < 0 && value == 0);
	assert(fd_read_int_checked(fd, &value) < 0 && value == 0);

	assert(close(fd) == 0);
	assert(unlink(path) == 0);
}

static void test_incomplete_idle_state_data_stays_unavailable(void)
{
	struct idle_state states[2];
	const struct idle_state *matrix[1] = {states};
	double out[MAX_VISIBLE_IDLE_STATES];
	int visible[MAX_VISIBLE_IDLE_STATES] = {1, 1};

	memset(states, 0, sizeof(states));
	states[0].available = 1;
	states[0].percentage = 20.0;
	states[1].available = 1;
	states[1].percentage = NAN;

	compute_idle_state_display(out, matrix, 0, 2, 80.0, visible, 0);
	assert(isnan(out[0]));
	assert(isnan(out[1]));

	compute_idle_state_display(out, matrix, 0, 2, 80.0, visible, 1);
	assert(isnan(out[0]));
	assert(isnan(out[1]));
}

static void test_cpuidle_interval_delta_helpers(void)
{
	assert(isnan(cpuidle_residency_percent(120, 1, 100, 0, 1000000)));
	assert(isnan(cpuidle_residency_percent(90, 1, 100, 1, 1000000)));
	assert(isnan(cpuidle_residency_percent(120, 1, 100, 1, 0)));
	assert(cpuidle_residency_percent(600000, 1, 100000, 1,
					 1000000) == 50.0);
	assert(cpuidle_residency_percent(2100000, 1, 100000, 1,
					 1000000) == 100.0);

	assert(isnan(cpuidle_usage_per_sec(12, 0, 10, 1, 1000000)));
	assert(isnan(cpuidle_usage_per_sec(8, 1, 10, 1, 1000000)));
	assert(cpuidle_usage_per_sec(15, 1, 10, 1, 500000) == 10.0);
}

static double jiffies_percent(unsigned long long jiffies,
			      unsigned long long delta_us)
{
	unsigned long long delta_jiffies_us;

	delta_jiffies_us = (jiffies * 1000000ULL) / get_kernel_hz();
	return (double)delta_jiffies_us * 100.0 / (double)delta_us;
}

static void test_frequency_sample_handles_max_value(void)
{
	struct cpu_freq_info freq;
	struct sys_snapshot snap;
	struct interval_stats stats;
	double expected_mhz;

	seed_one_cpu();
	init_aggregator();
	memset(&freq, 0, sizeof(freq));
	memset(&snap, 0, sizeof(snap));
	snap.cpu_count = 1;
	snap.effective_cpu_count = 1;
	snap.freqs = &freq;
	freq.cur_freq_valid = 1;
	freq.cur_freq = UINT_MAX - 2U;
	calculate_interval_stats(&snap, &stats);

	freq.cur_freq = UINT_MAX;
	calculate_interval_stats(&snap, &stats);
	expected_mhz = UINT_MAX / 1000.0;
	assert(fabs(stats.per_cpu_mhz[0] - expected_mhz) < 0.001);
	assert(fabs(stats.avg_mhz - expected_mhz) < 0.001);
	cleanup_aggregator();
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

static void test_package_aggregation_does_not_silently_stop_at_sixteen(void)
{
	struct cpu_inventory_seed cpus[17];
	struct sys_snapshot snap;
	struct interval_stats stats;

	for (int i = 0; i < 17; i++) {
		cpus[i].cpu_id = i;
		cpus[i].present = 1;
		cpus[i].online = 1;
		cpus[i].package_id = 100 + i;
		cpus[i].core_id = 0;
		cpus[i].numa_node = 0;
	}
	assert(cpu_inventory_seed(cpus, 17) == 0);
	init_aggregator();
	memset(&snap, 0, sizeof(snap));
	snap.cpu_count = 17;
	snap.effective_cpu_count = 17;

	calculate_interval_stats(&snap, &stats);
	assert(stats.package_count == 17);
	assert(stats.packages[16].package_id == 116);
	assert(stats.packages[16].cpu_count == 1);
	cleanup_aggregator();
}

static void test_procstat_failure_is_unavailable_and_rebaselines(void)
{
	struct sys_snapshot snap;
	struct interval_stats stats;
	unsigned long long idle_jiffies;
	unsigned long long iowait_jiffies = 0;
	unsigned long long runtime_ns = 0;
	unsigned char procstat_valid = 1;

	seed_one_cpu();
	set_busy_source_mode(BUSY_SOURCE_PROCSTAT);
	init_aggregator();

	memset(&snap, 0, sizeof(snap));
	snap.cpu_count = 1;
	snap.effective_cpu_count = 1;
	snap.interval_delta_us = 1000000ULL;
	idle_jiffies = (unsigned long long)get_kernel_hz();
	snap.authoritative_idle_jiffies = &idle_jiffies;
	snap.authoritative_iowait_jiffies = &iowait_jiffies;
	snap.authoritative_procstat_valid = &procstat_valid;
	snap.authoritative_runtime_ns = &runtime_ns;

	/* Establish a good baseline. */
	calculate_interval_stats(&snap, &stats);

	/* A failed read must not become a false 100% busy interval. */
	procstat_valid = 0;
	calculate_interval_stats(&snap, &stats);
	assert(isnan(stats.per_cpu_idle[0]));
	assert(isnan(stats.per_cpu_busy[0]));
	assert(isnan(stats.avg_idle_percent));
	assert(isnan(stats.busy_percent));

	/* The first good reading after failure is a new baseline. */
	procstat_valid = 1;
	idle_jiffies += (unsigned long long)get_kernel_hz() * 2ULL;
	calculate_interval_stats(&snap, &stats);
	assert(isnan(stats.per_cpu_idle[0]));
	assert(isnan(stats.per_cpu_busy[0]));

	/* Normal interval math resumes on the following good reading. */
	idle_jiffies += (unsigned long long)get_kernel_hz();
	calculate_interval_stats(&snap, &stats);
	assert(fabs(stats.per_cpu_idle[0] - 100.0) < 0.01);
	assert(fabs(stats.per_cpu_busy[0]) < 0.01);

	cleanup_aggregator();
}

static void test_procstat_jiffy_conversion_overflow_is_unavailable(void)
{
	struct sys_snapshot snap;
	struct interval_stats stats;
	unsigned long long idle_jiffies = 0;
	unsigned long long iowait_jiffies = 0;
	unsigned long long runtime_ns = 0;
	unsigned char procstat_valid = 1;

	seed_one_cpu();
	set_busy_source_mode(BUSY_SOURCE_PROCSTAT);
	init_aggregator();
	memset(&snap, 0, sizeof(snap));
	snap.cpu_count = 1;
	snap.effective_cpu_count = 1;
	snap.interval_delta_us = 1000000ULL;
	snap.authoritative_idle_jiffies = &idle_jiffies;
	snap.authoritative_iowait_jiffies = &iowait_jiffies;
	snap.authoritative_procstat_valid = &procstat_valid;
	snap.authoritative_runtime_ns = &runtime_ns;

	calculate_interval_stats(&snap, &stats);
	idle_jiffies = ULLONG_MAX / 1000000ULL + 1ULL;
	iowait_jiffies = ULLONG_MAX / 1000000ULL + 1ULL;
	calculate_interval_stats(&snap, &stats);

	assert(isnan(stats.per_cpu_idle[0]));
	assert(isnan(stats.per_cpu_busy[0]));
	assert(isnan(stats.per_cpu_iowait[0]));
	assert(isnan(stats.avg_idle_percent));
	assert(isnan(stats.avg_iowait_percent));
	cleanup_aggregator();
}

static void test_proc_stat_idle_includes_iowait(void)
{
	int cpu_id = -1;
	unsigned long long idle = 0;
	unsigned long long iowait = 0;

	assert(sysstat_parse_cpu_line("cpu7 10 2 3 40 5 6 7 8 9\n",
				       &cpu_id, &idle, &iowait) == 0);
	assert(cpu_id == 7);
	assert(idle == 45);
	assert(iowait == 5);

	assert(sysstat_parse_cpu_line("cpu2 10 2 3 40\n",
				       &cpu_id, &idle, &iowait) == 0);
	assert(cpu_id == 2);
	assert(idle == 40);
	assert(iowait == 0);

	assert(sysstat_parse_cpu_line("cpu 10 2 3 40 5\n",
				       &cpu_id, &idle, &iowait) < 0);
	assert(sysstat_parse_cpu_line("cpu0 10 2\n",
				       &cpu_id, &idle, &iowait) < 0);
	assert(sysstat_parse_cpu_line("cpu0 10 2 3 -1\n",
				       &cpu_id, &idle, &iowait) < 0);
	assert(sysstat_parse_cpu_line(
		"cpu0 10 2 3 18446744073709551616\n",
		&cpu_id, &idle, &iowait) < 0);
	assert(sysstat_parse_cpu_line("cpu0 10 2 3 40x\n",
				       &cpu_id, &idle, &iowait) < 0);

	assert(sysstat_parse_cpu_line(
		"cpu0 0 0 0 18446744073709551615 1\n",
		&cpu_id, &idle, &iowait) == 0);
	assert(idle == ULLONG_MAX);
}

static void test_schedstat_runtime_uses_documented_field_seven(void)
{
	int cpu_id;
	unsigned long long runtime_ns;

	assert(sysstat_parse_schedstat_cpu_line(
		"cpu7 1 2 3 4 5 6 7000000000 8 9\n",
		&cpu_id, &runtime_ns) == 0);
	assert(cpu_id == 7);
	assert(runtime_ns == 7000000000ULL);

	assert(sysstat_parse_schedstat_cpu_line(
		"cpu7 1 2 3 4 5 6 7000000000 8\n",
		&cpu_id, &runtime_ns) < 0);
	assert(sysstat_parse_schedstat_cpu_line(
		"cpu7 1 2 3 4 5 6 7000000000 8 9 junk\n",
		&cpu_id, &runtime_ns) < 0);
	assert(sysstat_parse_schedstat_cpu_line(
		"cpu+7 1 2 3 4 5 6 7000000000 8 9\n",
		&cpu_id, &runtime_ns) < 0);
	assert(sysstat_parse_schedstat_cpu_line(
		"cpu999999999999999999999 1 2 3 4 5 6 7 8 9\n",
		&cpu_id, &runtime_ns) < 0);
	assert(sysstat_parse_schedstat_cpu_line(
		"cpu7 1 2 3 4 5 6 -1 8 9\n",
		&cpu_id, &runtime_ns) < 0);
	assert(sysstat_parse_schedstat_cpu_line(
		"cpu7 1 2 3 4 5 6 18446744073709551616 8 9\n",
		&cpu_id, &runtime_ns) < 0);
}

static void test_sysstat_reader_rejects_invalid_arguments(void)
{
	unsigned long long values[MAX_CPUS];
	unsigned char valid[MAX_CPUS];

	assert(read_ctx_switches(NULL) < 0);
	assert(read_interrupts(NULL) < 0);
	assert(read_soft_interrupts(NULL) < 0);
	assert(read_all_proc_stat_cpu_idle_checked(values, values, valid, 0) < 0);
	assert(read_all_proc_stat_cpu_idle_checked(values, values, valid,
						   MAX_CPUS + 1) < 0);
	assert(read_all_schedstat_cpu_runtime_checked(values, valid, -1) < 0);
	assert(read_all_schedstat_cpu_runtime_checked(values, valid,
						      MAX_CPUS + 1) < 0);
}

static void test_power_failure_is_unavailable_and_rebaselines(void)
{
	reset_energy();
	update_power_interval_stats(0, 100000, 1);
	assert(fabs(get_interval_avg_power_mw() - 100000.0) < 0.01);
	assert(fabs(get_interval_energy_joules()) < 0.01);

	update_power_interval_stats(1000000, 120000, 1);
	assert(fabs(get_interval_avg_power_mw() - 110000.0) < 0.01);
	assert(fabs(get_interval_energy_joules() - 110.0) < 0.01);

	update_power_interval_stats(1000000, 0, 0);
	assert(isnan(get_interval_avg_power_mw()));
	assert(isnan(get_interval_energy_joules()));

	update_power_interval_stats(1000000, 130000, 1);
	assert(isnan(get_interval_avg_power_mw()));
	assert(isnan(get_interval_energy_joules()));

	update_power_interval_stats(1000000, 140000, 1);
	assert(fabs(get_interval_avg_power_mw() - 135000.0) < 0.01);
	assert(fabs(get_interval_energy_joules() - 135.0) < 0.01);
}

static void test_membw_failure_is_unavailable_and_rebaselines(void)
{
	reset_mem_bw();
	update_mem_bw_interval_stats(0, 1000, 1);
	assert(fabs(get_interval_mem_bw()) < 0.01);

	update_mem_bw_interval_stats(1000000, 1000 + 1024 * 1024, 1);
	assert(fabs(get_interval_mem_bw() - 1.0) < 0.01);

	update_mem_bw_interval_stats(1000000, 0, 0);
	assert(isnan(get_interval_mem_bw()));

	update_mem_bw_interval_stats(1000000, 1000 + 3 * 1024 * 1024, 1);
	assert(isnan(get_interval_mem_bw()));

	update_mem_bw_interval_stats(1000000, 1000 + 5 * 1024 * 1024, 1);
	assert(fabs(get_interval_mem_bw() - 2.0) < 0.01);

	/* Counter reset is also a new baseline, never a fabricated zero rate. */
	update_mem_bw_interval_stats(1000000, 10, 1);
	assert(isnan(get_interval_mem_bw()));
}

static void test_absolute_sampling_deadline_arithmetic(void)
{
	unsigned long long deadline;

	assert(sampling_deadline_init(1000, 100, &deadline) == 0);
	assert(deadline == 1100);

	/* Normal work before the next slot preserves the original phase. */
	assert(sampling_deadline_advance(&deadline, 100, 1150) == 0);
	assert(deadline == 1200);

	/* An expired slot is skipped instead of triggering catch-up sampling. */
	assert(sampling_deadline_advance(&deadline, 100, 1300) == 0);
	assert(deadline == 1400);

	/* Multiple missed periods advance directly to the first future slot. */
	assert(sampling_deadline_advance(&deadline, 100, 1755) == 0);
	assert(deadline == 1800);

	assert(sampling_deadline_init(0, 0, &deadline) < 0);
	assert(sampling_deadline_init(ULLONG_MAX, 1, &deadline) < 0);
	deadline = ULLONG_MAX - 50;
	assert(sampling_deadline_advance(&deadline, 100, 0) < 0);
	assert(deadline == ULLONG_MAX - 50);
}

static void test_sysstat_failure_is_unavailable_and_rebaselines(void)
{
	struct sys_snapshot snap;
	struct interval_stats stats;

	seed_one_cpu();
	init_aggregator();
	memset(&snap, 0, sizeof(snap));
	snap.cpu_count = 1;
	snap.effective_cpu_count = 1;
	snap.interval_delta_us = 1000000ULL;
	snap.counters.ctx_switches = 100;
	snap.counters.interrupts = 200;
	snap.counters.soft_interrupts = 300;
	snap.counters.sysstat_valid = 1;
	calculate_interval_stats(&snap, &stats);

	snap.counters.ctx_switches += 10;
	snap.counters.interrupts += 20;
	snap.counters.soft_interrupts += 30;
	calculate_interval_stats(&snap, &stats);
	assert(stats.ctx_switches == 10.0);
	assert(stats.interrupts == 20.0);
	assert(stats.soft_interrupts == 30.0);

	snap.counters.sysstat_valid = 0;
	calculate_interval_stats(&snap, &stats);
	assert(isnan(stats.ctx_switches));
	assert(isnan(stats.interrupts));
	assert(isnan(stats.soft_interrupts));

	snap.counters.sysstat_valid = 1;
	snap.counters.ctx_switches += 40;
	snap.counters.interrupts += 50;
	snap.counters.soft_interrupts += 60;
	calculate_interval_stats(&snap, &stats);
	assert(isnan(stats.ctx_switches));

	snap.counters.ctx_switches += 1;
	snap.counters.interrupts += 2;
	snap.counters.soft_interrupts += 3;
	calculate_interval_stats(&snap, &stats);
	assert(stats.ctx_switches == 1.0);
	assert(stats.interrupts == 2.0);
	assert(stats.soft_interrupts == 3.0);

	cleanup_aggregator();
}

static void test_pmu_failure_and_counter_reset_are_unavailable(void)
{
	struct sys_snapshot snap;
	struct interval_stats stats;
	uint64_t per_cpu[1][MAX_PMU_EVENTS] = {{0}};
	unsigned char per_cpu_valid[1] = {1};

	seed_one_cpu();
	init_aggregator();
	memset(&snap, 0, sizeof(snap));
	snap.cpu_count = 1;
	snap.effective_cpu_count = 1;
	snap.interval_delta_us = 1000000ULL;
	snap.counters.pmu_count = 1;
	snap.counters.pmu_per_cpu = per_cpu;
	snap.counters.pmu_per_cpu_valid = per_cpu_valid;
	snap.counters.pmu_valid = 1;
	snap.counters.pmu[0] = 100;
	per_cpu[0][0] = 100;

	calculate_interval_stats(&snap, &stats);
	assert(stats.pmu_valid == 1);
	assert(stats.per_cpu_pmu_valid[0] == 1);
	assert(stats.pmu_delta[0] == 100);
	assert(stats.per_cpu_pmu[0][0] == 100);

	snap.counters.pmu_valid = 0;
	per_cpu_valid[0] = 0;
	calculate_interval_stats(&snap, &stats);
	assert(stats.pmu_valid == 0);
	assert(stats.per_cpu_pmu_valid[0] == 0);

	snap.counters.pmu_valid = 1;
	per_cpu_valid[0] = 1;
	snap.counters.pmu[0] = 125;
	per_cpu[0][0] = 125;
	calculate_interval_stats(&snap, &stats);
	assert(stats.pmu_valid == 1);
	assert(stats.per_cpu_pmu_valid[0] == 1);
	assert(stats.pmu_delta[0] == 25);
	assert(stats.per_cpu_pmu[0][0] == 25);

	/* A reset/wrap must never be rendered as a zero-valued valid sample. */
	snap.counters.pmu[0] = 1;
	per_cpu[0][0] = 1;
	calculate_interval_stats(&snap, &stats);
	assert(stats.pmu_valid == 0);
	assert(stats.per_cpu_pmu_valid[0] == 0);

	cleanup_aggregator();
}

static void test_ipc_requires_cycles_and_instructions(void)
{
	struct sys_snapshot snap;
	struct interval_stats stats;

	seed_one_cpu();
	init_aggregator();
	(void)init_pmu_events("cycles");
	assert(get_pmu_event_count() == 1);
	memset(&snap, 0, sizeof(snap));
	snap.cpu_count = 1;
	snap.effective_cpu_count = 1;
	snap.interval_delta_us = 1000000ULL;
	snap.counters.pmu_count = 1;
	snap.counters.pmu_valid = 1;
	snap.counters.pmu[0] = 100;

	calculate_interval_stats(&snap, &stats);
	assert(stats.pmu_valid == 1);
	assert(isnan(stats.ipc));

	close_pmu_events();
	cleanup_aggregator();
}

static void test_pmu_raises_soft_fd_limit_for_large_tracked_set(void)
{
	pid_t pid = fork();
	int status;

	assert(pid >= 0);
	if (pid == 0) {
		struct cpu_inventory_seed cpus[64];
		struct rlimit limit;
		struct rlimit lowered;
		rlim_t expected = 64 * 2 + 64;

		if (getrlimit(RLIMIT_NOFILE, &limit) < 0 || limit.rlim_max < 64)
			_exit(0);
		lowered = limit;
		lowered.rlim_cur = 64;
		if (setrlimit(RLIMIT_NOFILE, &lowered) < 0)
			_exit(0);

		for (int i = 0; i < 64; i++) {
			cpus[i].cpu_id = i;
			cpus[i].present = 1;
			cpus[i].online = 1;
			cpus[i].package_id = 0;
			cpus[i].core_id = i;
			cpus[i].numa_node = 0;
		}
		cpu_inventory_seed(cpus, 64);
		(void)init_pmu_events("cycles,instructions");
		if (getrlimit(RLIMIT_NOFILE, &limit) < 0)
			_exit(1);
		if (limit.rlim_max != RLIM_INFINITY && expected > limit.rlim_max)
			expected = limit.rlim_max;
		close_pmu_events();
		_exit(limit.rlim_cur >= expected ? 0 : 1);
	}

	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
	test_checked_numeric_readers_reject_malformed_values();
	test_incomplete_idle_state_data_stays_unavailable();
	test_cpuidle_interval_delta_helpers();
	test_frequency_sample_handles_max_value();
	test_aggregator_idle_100_pct();
	test_aggregator_busy_50_pct();
	test_aggregator_iowait_independent();
	test_aggregator_schedstat_clamp();
	test_aggregator_sparse_package_ids();
	test_package_aggregation_does_not_silently_stop_at_sixteen();
	test_procstat_failure_is_unavailable_and_rebaselines();
	test_procstat_jiffy_conversion_overflow_is_unavailable();
	test_proc_stat_idle_includes_iowait();
	test_schedstat_runtime_uses_documented_field_seven();
	test_sysstat_reader_rejects_invalid_arguments();
	test_power_failure_is_unavailable_and_rebaselines();
	test_membw_failure_is_unavailable_and_rebaselines();
	test_absolute_sampling_deadline_arithmetic();
	test_sysstat_failure_is_unavailable_and_rebaselines();
	test_pmu_failure_and_counter_reset_are_unavailable();
	test_ipc_requires_cycles_and_instructions();
	test_pmu_raises_soft_fd_limit_for_large_tracked_set();

	printf("test_core_logic: all tests passed\n");
	return 0;
}
