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
#include <sys/stat.h>

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
static unsigned long long slow_time_us = 1000000;
static unsigned long long slow_read_cost_us;
static unsigned long long idle_read_cost_us;
static int config_failed;
static int config_representatives[MAX_CPUS];
static unsigned long long config_generations[MAX_CPUS];

static int fixture_slow_clock(clockid_t clock, struct timespec *now)
{
	(void)clock;
	now->tv_sec = (time_t)(slow_time_us / 1000000);
	now->tv_nsec = (long)(slow_time_us % 1000000) * 1000;
	return 0;
}

static void fixture_config_sweep(void) { }
static int fixture_config_representative(int cpu) { return config_representatives[cpu]; }
static unsigned long long fixture_config_generation(int cpu) { return config_generations[cpu]; }
static int fixture_config_matches(int cpu, int representative)
{
	return config_representatives[cpu] == config_representatives[representative];
}

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
	slow_time_us += (unsigned long long)(!!want_min + !!want_max) * slow_read_cost_us;
	*min = 1000000;
	*max = 3000000;
	*min_valid = !!want_min && !config_failed;
	*max_valid = !!want_max && !config_failed;
	return 0;
}

static int fixture_read_governor(int cpu, char *governor, size_t len)
{
	(void)cpu;
	governor_reads++;
	slow_time_us += slow_read_cost_us;
	if (config_failed)
		return -1;
	copy_cstring(governor, len, "performance");
	return 0;
}

static int fixture_read_boost(int cpu, int *value)
{
	(void)cpu;
	boost_reads++;
	slow_time_us += slow_read_cost_us;
	if (config_failed)
		return -1;
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
static void fixture_refresh_disable(int cpus)
{
	disable_refreshes += cpus;
	slow_time_us += (unsigned long long)cpus * idle_read_cost_us;
}
int fixture_init_sample_cache(int count);
void fixture_free_sample_cache(void);
int fixture_realloc_sample_cache(int count);
void fixture_collect_per_interval_data_raw(struct sys_snapshot *snapshot,
					   unsigned long long delta_us);
void fixture_collect_per_interval_data(struct sys_snapshot *snapshot,
				       unsigned long long delta_us);
void fixture_slow_stats(struct sample_cache_slow_stats *stats);
int fixture_cache_initialized(void);
void fixture_diagnostic_collection(int enable);

/* Rename only this included implementation, leaving the ordinary test link intact. */
#define init_sample_cache fixture_init_sample_cache
#define free_sample_cache fixture_free_sample_cache
#define realloc_sample_cache fixture_realloc_sample_cache
#define collect_per_interval_data fixture_collect_per_interval_data_raw
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
#define cpufreq_begin_config_sweep fixture_config_sweep
#define get_cpufreq_config_representative fixture_config_representative
#define get_cpufreq_config_generation fixture_config_generation
#define cpufreq_config_group_matches fixture_config_matches
#define get_sample_cache_slow_stats fixture_slow_stats
#define clock_gettime fixture_slow_clock
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
#undef cpufreq_begin_config_sweep
#undef get_cpufreq_config_representative
#undef get_cpufreq_config_generation
#undef cpufreq_config_group_matches
#undef get_sample_cache_slow_stats
#undef clock_gettime

void fixture_collect_per_interval_data(struct sys_snapshot *snapshot,
				       unsigned long long delta_us)
{
	slow_time_us += delta_us;
	fixture_collect_per_interval_data_raw(snapshot, delta_us);
}

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
	for (int i = 0; i < count; i++) {
		config_representatives[i] = i;
		config_generations[i] = 1;
	}
}

static void reset_reads(void)
{
	current_reads = min_reads = max_reads = governor_reads = boost_reads = 0;
	uncore_reads = schedstat_reads = disable_refreshes = budget_calculations = 0;
	slow_read_cost_us = idle_read_cost_us = 0;
	config_failed = 0;
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

static void test_policy_sharing_failure_age_and_recovery(void)
{
	struct sys_snapshot snapshot = { .interval_delta_us = 1000 };
	struct sample_cache_slow_stats stats;

	seed_cpus(4);
	for (int i = 0; i < 4; i++)
		config_representatives[i] = 0;
	configure_view("min_freq_mhz,max_freq_mhz,governor,boost");
	reset_reads();
	assert(fixture_init_sample_cache(4) == 0);
	assert(min_reads == 1 && max_reads == 1 && governor_reads == 1 && boost_reads == 1);
	fixture_collect_per_interval_data(&snapshot, 1000);
	for (int i = 0; i < 4; i++) {
		assert(snapshot.freqs[i].min_freq_valid && snapshot.freqs[i].max_freq_valid);
		assert(snapshot.freqs[i].boost == 1);
		assert(!strcmp(snapshot.freqs[i].governor, "performance"));
	}
	config_failed = 1;
	fixture_collect_per_interval_data(&snapshot, 1250000);
	fixture_collect_per_interval_data(&snapshot, 1);
	for (int i = 0; i < 4; i++) {
		assert(!snapshot.freqs[i].min_freq_valid && !snapshot.freqs[i].max_freq_valid);
		assert(snapshot.freqs[i].boost == -1 && !snapshot.freqs[i].governor[0]);
	}
	fixture_slow_stats(&stats);
	assert(stats.unavailable_cpus == 4 && stats.max_age_us >= 1250000);
	config_failed = 0;
	fixture_collect_per_interval_data(&snapshot, 5000000);
	fixture_collect_per_interval_data(&snapshot, 1);
	assert(snapshot.freqs[3].min_freq_valid && snapshot.freqs[3].boost == 1);

	/* A clock gap with a short requested sample exposes old config as stale. */
	slow_time_us += 10000001;
	fixture_collect_per_interval_data(&snapshot, 1);
	fixture_slow_stats(&stats);
	assert(stats.stale_cpus == 4 && stats.unavailable_cpus == 4);
	assert(!snapshot.freqs[0].min_freq_valid && snapshot.freqs[0].boost == -1);
	assert(!snapshot.freqs[0].governor[0]);
	/* A legitimate long sampling interval receives a matching freshness window. */
	fixture_collect_per_interval_data(&snapshot, 30000000);
	assert(snapshot.freqs[0].min_freq_valid);
	fixture_free_sample_cache();
}

static void test_time_budget_retains_credit_and_replaces_cpu_cap(void)
{
	struct sys_snapshot snapshot = { .interval_delta_us = 1000000 };
	struct sample_cache_slow_stats stats;

	seed_cpus(MAX_CPUS);
	configure_view("min_freq_mhz");
	reset_reads();
	assert(fixture_init_sample_cache(MAX_CPUS) == 0);
	reset_reads();
	fixture_collect_per_interval_data(&snapshot, 1000000);
	fixture_slow_stats(&stats);
	assert(stats.refreshed_cpus == 204 && min_reads == 204);
	assert(stats.time_budget_us == 2000);
	fixture_free_sample_cache();

	seed_cpus(100);
	configure_view("min_freq_mhz,idle_state0");
	reset_reads();
	assert(fixture_init_sample_cache(100) == 0);
	reset_reads();
	slow_read_cost_us = 100;
	idle_read_cost_us = 100;
	fixture_collect_per_interval_data(&snapshot, 1000000);
	fixture_slow_stats(&stats);
	assert(stats.refreshed_cpus == 10 && min_reads == 10 && disable_refreshes == 10);
	assert(stats.work_us == 2000 && stats.time_budget_us == 2000);
	assert(slow_budget_credit / 5000000 == 10);
	for (int i = 0; i < 9; i++)
		fixture_collect_per_interval_data(&snapshot, 1000000);
	fixture_slow_stats(&stats);
	assert(stats.last_sweep_us >= 10000000);
	assert(stats.last_sweep_us < 10100000);
	fixture_free_sample_cache();
}

/* Real policy discovery is exercised with independently controlled directory IDs. */
static ino_t fixture_policy_inodes[MAX_CPUS];
static int fixture_policy_stats;

static int fixture_policy_stat(const char *path, struct stat *st)
{
	const char *cpu = strstr(path, "/cpu/cpu");
	int cpu_id = -1;

	fixture_policy_stats++;
	if (!cpu || sscanf(cpu, "/cpu/cpu%d/cpufreq", &cpu_id) != 1 ||
	    cpu_id < 0 || cpu_id >= MAX_CPUS || !fixture_policy_inodes[cpu_id]) {
		errno = ENOENT;
		return -1;
	}
	memset(st, 0, sizeof(*st));
	st->st_dev = 7;
	st->st_ino = fixture_policy_inodes[cpu_id];
	st->st_mode = S_IFDIR | 0755;
	return 0;
}

#define read_cpu_freq fixture_backend_read_cpu_freq
#define read_cpu_min_max_freq_checked fixture_backend_read_cpu_min_max_freq_checked
#define read_cpu_min_max_freq_selected fixture_backend_read_cpu_min_max_freq_selected
#define configure_cpufreq_fd_cache fixture_backend_configure_cpufreq_fd_cache
#define get_cpufreq_cached_fd_count fixture_backend_get_cpufreq_cached_fd_count
#define cpufreq_begin_sample fixture_backend_cpufreq_begin_sample
#define cpufreq_begin_config_sweep fixture_backend_cpufreq_begin_config_sweep
#define get_cpufreq_config_representative fixture_backend_get_cpufreq_config_representative
#define cpufreq_config_group_matches fixture_backend_cpufreq_config_group_matches
#define get_cpufreq_config_generation fixture_backend_get_cpufreq_config_generation
#define read_cpu_governor fixture_backend_read_cpu_governor
#define read_cpu_boost fixture_backend_read_cpu_boost
#define read_uncore_freq fixture_backend_read_uncore_freq
#define has_uncore_freq_support fixture_backend_has_uncore_freq_support
#define get_uncore_freq_device_name fixture_backend_get_uncore_freq_device_name
#define init_cpufreq fixture_backend_init_cpufreq
#define get_cpufreq_tracked_count fixture_backend_get_cpufreq_tracked_count
#define close_cpufreq fixture_backend_close_cpufreq
/* Read current CPU frequency in kHz (tracked_idx) */
int read_cpu_freq(int tracked_idx, unsigned int *freq);

/* Read min/max frequency with independent validity for min and max. */
int read_cpu_min_max_freq_checked(int tracked_idx,
				  unsigned int *min, int *min_valid,
				  unsigned int *max, int *max_valid);
int read_cpu_min_max_freq_selected(int tracked_idx,
				 unsigned int *min, int *min_valid,
				 unsigned int *max, int *max_valid,
				 int read_min, int read_max);

/* Set the persistent current-frequency cache share; existing fds may shrink. */
void configure_cpufreq_fd_cache(int fd_cap, int collect_current);
int get_cpufreq_cached_fd_count(void);
void cpufreq_begin_sample(void);

/* Slow configuration sharing requires a verified common sysfs directory. */
void cpufreq_begin_config_sweep(void);
int get_cpufreq_config_representative(int tracked_idx);
int cpufreq_config_group_matches(int tracked_idx, int representative);
unsigned long long get_cpufreq_config_generation(int tracked_idx);

/* Read CPU governor (tracked_idx) */
int read_cpu_governor(int tracked_idx, char *governor, size_t len);

/* Read CPU boost state (0/1), returns -1 if unavailable (tracked_idx) */
int read_cpu_boost(int tracked_idx, int *boost);

/* Read uncore frequency from devfreq cur_freq in Hz */
int read_uncore_freq(unsigned long long *freq_hz);

/* Whether a readable devfreq cur_freq source was discovered */
int has_uncore_freq_support(void);

/* Return the discovered devfreq device name, or NULL if unavailable */
const char *get_uncore_freq_device_name(void);

/* Initialize cpufreq subsystem */
int init_cpufreq(void);

/* Number of tracked CPUs represented by the current cached-fd set. */
int get_cpufreq_tracked_count(void);

/* Close cached file descriptors */
void close_cpufreq(void);

#define stat(path, st) fixture_policy_stat(path, st)
#include "../src/platform/cpufreq.c"
#undef stat
#undef read_cpu_freq
#undef read_cpu_min_max_freq_checked
#undef read_cpu_min_max_freq_selected
#undef configure_cpufreq_fd_cache
#undef get_cpufreq_cached_fd_count
#undef cpufreq_begin_sample
#undef cpufreq_begin_config_sweep
#undef get_cpufreq_config_representative
#undef cpufreq_config_group_matches
#undef get_cpufreq_config_generation
#undef read_cpu_governor
#undef read_cpu_boost
#undef read_uncore_freq
#undef has_uncore_freq_support
#undef get_uncore_freq_device_name
#undef init_cpufreq
#undef get_cpufreq_tracked_count
#undef close_cpufreq

static void test_real_policy_identity_verification_and_replacement(void)
{
	unsigned long long old_generation;
	int checks;

	seed_cpus(4);
	fixture_policy_inodes[0] = fixture_policy_inodes[1] = 100;
	fixture_policy_inodes[2] = 200;
	fixture_policy_inodes[3] = 0;
	fixture_policy_stats = 0;
	assert(fixture_backend_init_cpufreq() == 0);
	assert(fixture_policy_stats == 4);
	checks = fixture_policy_stats;
	assert(fixture_backend_get_cpufreq_config_representative(0) == 0);
	assert(fixture_backend_get_cpufreq_config_representative(1) == 0);
	assert(fixture_backend_get_cpufreq_config_representative(2) == 2);
	assert(fixture_backend_get_cpufreq_config_representative(3) == 3);
	assert(fixture_policy_stats == checks);
	assert(!fixture_backend_cpufreq_config_group_matches(3, 0));

	old_generation = fixture_backend_get_cpufreq_config_generation(0);
	fixture_policy_inodes[0] = 300;
	fixture_backend_cpufreq_begin_config_sweep();
	/* CPU 1 must verify its previous representative before trusting the cache. */
	assert(fixture_backend_get_cpufreq_config_representative(1) == 1);
	assert(fixture_policy_stats == checks + 2);
	assert(!fixture_backend_cpufreq_config_group_matches(1, 0));
	assert(fixture_backend_get_cpufreq_config_generation(0) != old_generation);
	fixture_policy_inodes[3] = 300;
	assert(fixture_backend_get_cpufreq_config_representative(3) == 0);
	assert(fixture_backend_cpufreq_config_group_matches(3, 0));
	fixture_backend_close_cpufreq();
	assert(fixture_backend_get_cpufreq_tracked_count() == 0);
}

static void test_independent_policy_index_at_cpu_limit(void)
{
	unsigned int occupied = 0;

	seed_cpus(MAX_CPUS);
	for (int i = 0; i < MAX_CPUS; i++)
		fixture_policy_inodes[i] = (ino_t)(10000 + i);
	fixture_policy_stats = 0;
	assert(fixture_backend_init_cpufreq() == 0);
	assert(policy_index_size >= MAX_CPUS * 2);
	for (unsigned int i = 0; i < policy_index_size; i++)
		occupied += !!policy_index[i].occupied;
	assert(occupied == MAX_CPUS);
	fixture_backend_cpufreq_begin_config_sweep();
	for (int i = 0; i < MAX_CPUS; i++) {
		assert(fixture_backend_get_cpufreq_config_representative(i) == i);
		assert(policy_identity[i].representative == i);
	}
	/* Exactly one directory verification per CPU, including independent ones. */
	assert(fixture_policy_stats == MAX_CPUS * 2);
	fixture_backend_close_cpufreq();
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
	test_policy_sharing_failure_age_and_recovery();
	test_time_budget_retains_credit_and_replaces_cpu_cap();
	test_real_policy_identity_verification_and_replacement();
	test_independent_policy_index_at_cpu_limit();
	test_counter_missing_permission_backoff_and_reset();
	test_counter_cached_ephemeral_recovery_and_cloexec();
	test_fd_budget_reserves_runtime_space();
	cleanup_cpu_inventory();
	puts("sample collection tests: 10 cases passed");
	return 0;
}
