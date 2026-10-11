// SPDX-License-Identifier: GPL-2.0
/* Read-count regressions use the real sample-cache pipeline with read hooks. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

#include "armstat_cli.h"
#include "sample_cache.h"
#include "sysfs_util.h"
#include "sysstat.h"
#include "idle_backend.h"
#include "formatter.h"
#include "formatter_section.h"
#include "cpu_inventory.h"

static int current_reads;
static int min_reads;
static int max_reads;
static int governor_reads;
static int boost_reads;
static int uncore_reads;
static int schedstat_reads;
static int disable_refreshes;
static int budget_calculations;
static int freq_fd_cap;
static int idle_fd_cap;
static unsigned int last_residency_mask;
static unsigned int last_usage_mask;
static int mock_budget = 128;
static int mock_schedstat_failed = 1;

static int fixture_read_freq(int cpu, unsigned int *value)
{
	(void)cpu;
	current_reads++;
	*value = 2400000;
	return 0;
}

static int fixture_read_min_max(int cpu, unsigned int *min, int *min_valid,
				unsigned int *max, int *max_valid,
				int want_min, int want_max)
{
	(void)cpu;
	min_reads += !!want_min;
	max_reads += !!want_max;
	*min = 1000000;
	*max = 3000000;
	*min_valid = !!want_min;
	*max_valid = !!want_max;
	return 0;
}

static int fixture_read_governor(int cpu, char *governor, size_t len)
{
	(void)cpu;
	governor_reads++;
	copy_cstring(governor, len, "performance");
	return 0;
}

static int fixture_read_boost(int cpu, int *value)
{
	(void)cpu;
	boost_reads++;
	*value = 1;
	return 0;
}

static int fixture_read_uncore(unsigned long long *value)
{
	uncore_reads++;
	*value = 1000000000;
	return 0;
}

static int fixture_read_schedstat(unsigned long long *values,
				  unsigned char *valid, int max_cpus)
{
	schedstat_reads++;
	if (mock_schedstat_failed)
		return -1;
	for (int i = 0; i < max_cpus; i++) {
		values[i] = 100000;
		valid[i] = 1;
	}
	return max_cpus;
}

static int fixture_read_procstat(unsigned long long *idle,
				unsigned long long *iowait,
				unsigned char *valid, int max_cpus)
{
	for (int i = 0; i < max_cpus; i++) {
		idle[i] = 100;
		iowait[i] = 0;
		valid[i] = 1;
	}
	return max_cpus;
}

static int fixture_idle_enabled(void) { return 1; }
static int fixture_idle_count(void) { return MAX_IDLE_STATES; }
static struct idle_state **fixture_idle_array(void) { return NULL; }
static int fixture_cached_count(void) { return 0; }
static int fixture_fd_budget(int requested, int cached)
{
	(void)cached;
	budget_calculations++;
	return requested < mock_budget ? requested : mock_budget;
}
static void fixture_configure_freq(int cap, int current)
{
	freq_fd_cap = current ? cap : 0;
}
static void fixture_configure_idle(int cap, unsigned int residency, unsigned int usage)
{
	idle_fd_cap = cap;
	last_residency_mask = residency;
	last_usage_mask = usage;
}
static void fixture_refresh_disable(int cpus) { disable_refreshes += cpus; }
int fixture_init_sample_cache(int count);
void fixture_free_sample_cache(void);
int fixture_realloc_sample_cache(int count);
void fixture_collect_per_interval_data(struct sys_snapshot *snapshot,
				       unsigned long long delta_us);
int fixture_cache_initialized(void);
void fixture_diagnostic_collection(int enable);

/* Rename only this included implementation, leaving the ordinary test link intact. */
#define init_sample_cache fixture_init_sample_cache
#define free_sample_cache fixture_free_sample_cache
#define realloc_sample_cache fixture_realloc_sample_cache
#define collect_per_interval_data fixture_collect_per_interval_data
#define is_sample_cache_initialized fixture_cache_initialized
#define set_diagnostic_counter_collection fixture_diagnostic_collection
#define read_cpu_freq fixture_read_freq
#define read_cpu_min_max_freq_selected fixture_read_min_max
#define read_cpu_governor fixture_read_governor
#define read_cpu_boost fixture_read_boost
#define read_uncore_freq fixture_read_uncore
#define read_all_schedstat_cpu_runtime_checked fixture_read_schedstat
#define read_all_proc_stat_cpu_idle_checked fixture_read_procstat
#define is_cpuidle_enabled fixture_idle_enabled
#define get_global_idle_state_count fixture_idle_count
#define get_idle_states_array fixture_idle_array
#define get_cpufreq_cached_fd_count fixture_cached_count
#define get_cpuidle_cached_fd_count fixture_cached_count
#define configure_cpufreq_fd_cache fixture_configure_freq
#define configure_cpuidle_fd_cache fixture_configure_idle
#define refresh_idle_state_disable_cache_budgeted fixture_refresh_disable
#define sysfs_counter_fd_budget fixture_fd_budget
#include "../src/core/sample_cache.c"
#undef init_sample_cache
#undef free_sample_cache
#undef realloc_sample_cache
#undef collect_per_interval_data
#undef is_sample_cache_initialized
#undef set_diagnostic_counter_collection
#undef read_cpu_freq
#undef read_cpu_min_max_freq_selected
#undef read_cpu_governor
#undef read_cpu_boost
#undef read_uncore_freq
#undef read_all_schedstat_cpu_runtime_checked
#undef read_all_proc_stat_cpu_idle_checked
#undef is_cpuidle_enabled
#undef get_global_idle_state_count
#undef get_idle_states_array
#undef get_cpufreq_cached_fd_count
#undef get_cpuidle_cached_fd_count
#undef configure_cpufreq_fd_cache
#undef configure_cpuidle_fd_cache
#undef refresh_idle_state_disable_cache_budgeted
#undef sysfs_counter_fd_budget

static void seed_cpus(int count)
{
	struct cpu_inventory_seed cpus[MAX_CPUS];

	assert(set_cpu_inventory_filter(NULL) == 0);
	for (int i = 0; i < count; i++)
		cpus[i] = (struct cpu_inventory_seed){
			.cpu_id = i, .present = 1, .online = 1,
			.package_id = 0, .core_id = i, .numa_node = 0,
		};
	assert(cpu_inventory_seed(cpus, count) == 0);
}

static void reset_reads(void)
{
	current_reads = min_reads = max_reads = governor_reads = boost_reads = 0;
	uncore_reads = schedstat_reads = disable_refreshes = budget_calculations = 0;
}

static void configure_view(const char *fields)
{
	clear_columns();
	set_section_summary_mode(0);
	set_section_default_summary_output(1);
	set_section_package_output(0);
	parse_column_option(fields, 1);
	fixture_diagnostic_collection(0);
	set_busy_source_mode(BUSY_SOURCE_AUTO);
}

static void test_schedstat_failure_once_per_cycle_and_recovery(void)
{
	struct sys_snapshot snapshot = { .interval_delta_us = 1000 };

	seed_cpus(MAX_CPUS);
	configure_view("cpu_busy_percent");
	set_busy_source_mode(BUSY_SOURCE_SCHEDSTAT);
	reset_reads();
	assert(fixture_init_sample_cache(MAX_CPUS) == 0);
	mock_schedstat_failed = 1;
	fixture_collect_per_interval_data(&snapshot, 1000);
	assert(schedstat_reads == 1);
	for (int i = 0; i < MAX_CPUS; i++)
		assert(!snapshot.authoritative_runtime_valid[i]);
	mock_schedstat_failed = 0;
	fixture_collect_per_interval_data(&snapshot, 1000);
	assert(schedstat_reads == 2);
	for (int i = 0; i < MAX_CPUS; i++)
		assert(snapshot.authoritative_runtime_valid[i]);
	fixture_free_sample_cache();
	mock_schedstat_failed = 1;
}

static void test_frequency_dependency_and_fractional_slow_refresh(void)
{
	struct sys_snapshot snapshot = { .interval_delta_us = 1000 };

	seed_cpus(1);
	configure_view("summary_freq_mhz");
	reset_reads();
	assert(fixture_init_sample_cache(1) == 0);
	assert(min_reads == 0 && max_reads == 0 && !governor_reads && !boost_reads);
	for (int i = 0; i < 100; i++)
		fixture_collect_per_interval_data(&snapshot, 1000);
	assert(current_reads == 100 && uncore_reads == 0);
	assert(min_reads == 0 && max_reads == 0 && !governor_reads && !boost_reads);
	assert(budget_calculations == 1);
	fixture_free_sample_cache();

	configure_view("min_freq_mhz");
	reset_reads();
	assert(fixture_init_sample_cache(1) == 0);
	assert(min_reads == 1 && max_reads == 0 && !governor_reads && !boost_reads);
	for (int i = 0; i < 4999; i++)
		fixture_collect_per_interval_data(&snapshot, 1000);
	assert(min_reads == 1 && current_reads == 0 && uncore_reads == 0);
	fixture_collect_per_interval_data(&snapshot, 1000);
	assert(min_reads == 2 && max_reads == 0);
	fixture_free_sample_cache();

	configure_view("governor,boost");
	reset_reads();
	assert(fixture_init_sample_cache(1) == 0);
	assert(governor_reads == 1 && boost_reads == 1 && !min_reads && !max_reads);
	fixture_collect_per_interval_data(&snapshot, 1000);
	assert(!current_reads && !uncore_reads);
	fixture_free_sample_cache();
}

static void test_diagnostic_full_state_budget_and_hotplug_reset(void)
{
	struct sys_snapshot snapshot = { .interval_delta_us = 1000 };

	seed_cpus(4);
	configure_view("cpu_busy_percent");
	fixture_diagnostic_collection(1);
	reset_reads();
	assert(fixture_init_sample_cache(4) == 0);
	assert(min_reads == 4 && max_reads == 4 && governor_reads == 4 && boost_reads == 4);
	fixture_collect_per_interval_data(&snapshot, 1000);
	assert(current_reads == 4 && uncore_reads == 1 && schedstat_reads == 1);
	assert(last_residency_mask == (1U << MAX_IDLE_STATES) - 1U);
	assert(last_usage_mask == last_residency_mask);
	assert(freq_fd_cap + idle_fd_cap == mock_budget);
	assert(freq_fd_cap > 0 && idle_fd_cap > freq_fd_cap);
	assert(disable_refreshes == 0);
	fixture_collect_per_interval_data(&snapshot, 1249000);
	assert(disable_refreshes == 1);
	assert(budget_calculations == 1);
	seed_cpus(1);
	assert(fixture_realloc_sample_cache(1) == 0);
	fixture_collect_per_interval_data(&snapshot, 1000);
	assert(budget_calculations == 2);
	assert(freq_fd_cap + idle_fd_cap == 33);
	fixture_free_sample_cache();
	fixture_diagnostic_collection(0);
}

/* Counter-cache failure timing is deterministic and does not sleep. */
static uint64_t fake_time_us = 1000000;
static int counter_opens;
static int fake_open_error;

static int fixture_clock_gettime(clockid_t clock, struct timespec *now)
{
	(void)clock;
	now->tv_sec = (time_t)(fake_time_us / 1000000);
	now->tv_nsec = (long)(fake_time_us % 1000000) * 1000;
	return 0;
}

static int fixture_counter_open(const char *path, int flags)
{
	counter_opens++;
	if (fake_open_error) {
		errno = fake_open_error;
		return -1;
	}
	return open(path, flags);
}
void fixture_counter_init(struct sysfs_counter_cache *cache);
void fixture_counter_close(struct sysfs_counter_cache *cache, int *open_count);
int fixture_counter_read(struct sysfs_counter_cache *cache, const char *path,
			 unsigned long long *out, int *open_count, int fd_cap);
void fixture_counter_begin_sample(void);
void fixture_counter_end_sample(void);
int fixture_counter_sample_active(void);
int fixture_counter_fd_budget(int requested, int cached_fds);
int fixture_sysfs_read_int(const char *path, int *out);
int fixture_sysfs_read_ull(const char *path, unsigned long long *out);
char *fixture_sysfs_read_str(const char *path, char *buf, size_t len);
int fixture_sysfs_path_exists(const char *path);
int fixture_fd_read_ull(int fd, unsigned long long *out);
int fixture_fd_read_int(int fd, int *out);

#define sysfs_counter_init fixture_counter_init
#define sysfs_counter_close fixture_counter_close
#define sysfs_counter_read fixture_counter_read
#define sysfs_counter_begin_sample fixture_counter_begin_sample
#define sysfs_counter_end_sample fixture_counter_end_sample
#define sysfs_counter_sample_active fixture_counter_sample_active
#define sysfs_counter_fd_budget fixture_counter_fd_budget
#define sysfs_read_int_checked fixture_sysfs_read_int
#define sysfs_read_ull_checked fixture_sysfs_read_ull
#define sysfs_read_str fixture_sysfs_read_str
#define sysfs_path_exists fixture_sysfs_path_exists
#define fd_read_ull_checked fixture_fd_read_ull
#define fd_read_int_checked fixture_fd_read_int
#define open fixture_counter_open
#define clock_gettime fixture_clock_gettime
#include "../src/platform/sysfs_util.c"
#undef sysfs_counter_init
#undef sysfs_counter_close
#undef sysfs_counter_read
#undef sysfs_counter_begin_sample
#undef sysfs_counter_end_sample
#undef sysfs_counter_sample_active
#undef sysfs_counter_fd_budget
#undef sysfs_read_int_checked
#undef sysfs_read_ull_checked
#undef sysfs_read_str
#undef sysfs_path_exists
#undef fd_read_ull_checked
#undef fd_read_int_checked
#undef open
#undef clock_gettime

static void test_counter_missing_permission_backoff_and_reset(void)
{
	struct sysfs_counter_cache cache;
	unsigned long long value;
	int count = 0;

	fixture_counter_init(&cache);
	counter_opens = 0;
	fake_open_error = ENOENT;
	assert(fixture_counter_read(&cache, "/missing", &value, &count, 8) < 0);
	assert(counter_opens == 1);
	assert(fixture_counter_read(&cache, "/missing", &value, &count, 8) < 0);
	assert(counter_opens == 1);
	fake_time_us += 249999;
	assert(fixture_counter_read(&cache, "/missing", &value, &count, 8) < 0);
	assert(counter_opens == 1);
	fake_time_us++;
	assert(fixture_counter_read(&cache, "/missing", &value, &count, 8) < 0);
	assert(counter_opens == 2);
	assert(cache.retry_after_us == fake_time_us + 500000);
	fixture_counter_close(&cache, &count);
	fake_open_error = EACCES;
	assert(fixture_counter_read(&cache, "/permission", &value, &count, 8) < 0);
	assert(counter_opens == 3);
	assert(cache.retry_after_us == fake_time_us + 250000);
	for (int i = 0; i < 10; i++) {
		fake_time_us = cache.retry_after_us;
		assert(fixture_counter_read(&cache, "/permission", &value, &count, 8) < 0);
		assert(cache.retry_after_us - fake_time_us <= 5000000);
	}
	assert(cache.retry_after_us - fake_time_us == 5000000);
	fake_open_error = 0;
	fixture_counter_close(&cache, &count);
}

static void test_counter_cached_ephemeral_recovery_and_cloexec(void)
{
	char path[] = "/tmp/armstat-counter-test-XXXXXX";
	struct sysfs_counter_cache cache;
	unsigned long long value;
	int fd = mkstemp(path);
	int count = 0;

	assert(fd >= 0);
	assert(write(fd, "0\n", 2) == 2);
	fixture_counter_init(&cache);
	counter_opens = 0;
	assert(fixture_counter_read(&cache, NULL, &value, &count, 1) < 0);
	assert(counter_opens == 0 && count == 0);
	assert(fixture_counter_read(&cache, path, &value, &count, 1) == 0);
	assert(value == 0 && count == 1 && counter_opens == 1);
	assert(fcntl(cache.fd, F_GETFD) & FD_CLOEXEC);
	assert(pwrite(fd, "7\n", 2, 0) == 2);
	assert(fixture_counter_read(&cache, NULL, &value, &count, 1) == 0);
	assert(value == 7 && counter_opens == 1);
	assert(pwrite(fd, "x\n", 2, 0) == 2);
	assert(fixture_counter_read(&cache, path, &value, &count, 1) < 0);
	assert(counter_opens == 1 && count == 0 && cache.fd < 0);
	assert(pwrite(fd, "9\n", 2, 0) == 2);
	assert(fixture_counter_read(&cache, path, &value, &count, 1) == 0);
	assert(value == 9 && counter_opens == 2 && count == 1);
	fixture_counter_close(&cache, &count);
	assert(fixture_counter_read(&cache, path, &value, &count, 0) == 0);
	assert(fixture_counter_read(&cache, path, &value, &count, 0) == 0);
	assert(count == 0 && cache.fd < 0 && counter_opens == 4);
	assert(close(fd) == 0);
	assert(unlink(path) == 0);
	fixture_counter_close(&cache, &count);
}

static void test_fd_budget_reserves_runtime_space(void)
{
	struct rlimit before;
	struct rlimit small;
	int budget;

	assert(getrlimit(RLIMIT_NOFILE, &before) == 0);
	small = before;
	if (small.rlim_cur > 128)
		small.rlim_cur = 128;
	assert(setrlimit(RLIMIT_NOFILE, &small) == 0);
	budget = fixture_counter_fd_budget(10000, 0);
	assert(budget >= 0 && budget <= 64);
	small.rlim_cur = 64;
	if (small.rlim_cur <= before.rlim_max) {
		assert(setrlimit(RLIMIT_NOFILE, &small) == 0);
		assert(fixture_counter_fd_budget(10000, 0) == 0);
	}
	assert(setrlimit(RLIMIT_NOFILE, &before) == 0);
}

int main(void)
{
	test_schedstat_failure_once_per_cycle_and_recovery();
	test_frequency_dependency_and_fractional_slow_refresh();
	test_diagnostic_full_state_budget_and_hotplug_reset();
	test_counter_missing_permission_backoff_and_reset();
	test_counter_cached_ephemeral_recovery_and_cloexec();
	test_fd_budget_reserves_runtime_space();
	cleanup_cpu_inventory();
	puts("sample collection tests: 6 cases passed");
	return 0;
}
