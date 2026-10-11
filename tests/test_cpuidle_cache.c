// SPDX-License-Identifier: GPL-2.0
/* Exercise the real cpuidle implementation against a private sysfs tree. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "cpuidle.h"
#include "cpu_inventory.h"
#include "sysfs_util.h"

static char tree[128];
static int state_directory_probes;
static int time_15_opens;
static uint64_t fixture_now_us = 1000000;

static int fixture_cpu_path(int cpu, const char *sub, char *path, size_t len)
{
	int written = snprintf(path, len, "%s/cpu%d/%s", tree, cpu, sub);

	return written < 0 || (size_t)written >= len ? -1 : 0;
}

static DIR *fixture_opendir(const char *path)
{
	state_directory_probes++;
	return opendir(path);
}

static char *fixture_read_name(const char *path, char *buf, size_t len)
{
	const char *prefix = "/sys/devices/system/cpu/";
	char actual[256];
	int written;

	assert(strncmp(path, prefix, strlen(prefix)) == 0);
	written = snprintf(actual, sizeof(actual), "%s/%s", tree, path + strlen(prefix));
	assert(written > 0 && (size_t)written < sizeof(actual));
	return sysfs_read_str(actual, buf, len);
}

static int fixture_clock(clockid_t clock, struct timespec *now)
{
	(void)clock;
	now->tv_sec = (time_t)(fixture_now_us / 1000000);
	now->tv_nsec = (long)(fixture_now_us % 1000000) * 1000;
	return 0;
}

static int fixture_open(const char *path, int flags)
{
	if (strstr(path, "/cpu0/cpuidle/state15/time"))
		time_15_opens++;
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
int fixture_read_int(const char *path, int *out);
int fixture_read_ull(const char *path, unsigned long long *out);
char *fixture_read_str(const char *path, char *buf, size_t len);
int fixture_exists(const char *path);
int fixture_fd_read_ull(int fd, unsigned long long *out);
int fixture_fd_read_int(int fd, int *out);

#define sysfs_counter_init fixture_counter_init
#define sysfs_counter_close fixture_counter_close
#define sysfs_counter_read fixture_counter_read
#define sysfs_counter_begin_sample fixture_counter_begin_sample
#define sysfs_counter_end_sample fixture_counter_end_sample
#define sysfs_counter_sample_active fixture_counter_sample_active
#define sysfs_counter_fd_budget fixture_counter_fd_budget
#define sysfs_read_int_checked fixture_read_int
#define sysfs_read_ull_checked fixture_read_ull
#define sysfs_read_str fixture_read_str
#define sysfs_path_exists fixture_exists
#define fd_read_ull_checked fixture_fd_read_ull
#define fd_read_int_checked fixture_fd_read_int
#define open fixture_open
#define clock_gettime fixture_clock
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

int fixture_state_count(int cpu);
int fixture_init_cpuidle(void);
void fixture_close_cpuidle(void);
void fixture_enable_cpuidle(int enable);
int fixture_cpuidle_enabled(void);
double fixture_residency_percent(unsigned long long current, int current_valid,
				 unsigned long long previous, int previous_valid,
				 unsigned long long elapsed_us);
double fixture_usage_rate(unsigned long long current, int current_valid,
			 unsigned long long previous, int previous_valid,
			 unsigned long long elapsed_us);
void fixture_update_states(unsigned long long elapsed_us, unsigned int residency,
			   unsigned int usage);
void fixture_refresh_disable(int cpu_budget);
void fixture_configure_cache(int fd_cap, unsigned int residency, unsigned int usage);
int fixture_cached_count(void);
int fixture_global_count(void);
const char *fixture_state_name(int state);
struct idle_state **fixture_states_array(void);
void fixture_effective_count(int count);

#define get_idle_state_count fixture_state_count
#define init_cpuidle fixture_init_cpuidle
#define close_cpuidle fixture_close_cpuidle
#define enable_cpuidle fixture_enable_cpuidle
#define is_cpuidle_enabled fixture_cpuidle_enabled
#define cpuidle_residency_percent fixture_residency_percent
#define cpuidle_usage_per_sec fixture_usage_rate
#define update_idle_states fixture_update_states
#define refresh_idle_state_disable_cache_budgeted fixture_refresh_disable
#define configure_cpuidle_fd_cache fixture_configure_cache
#define get_cpuidle_cached_fd_count fixture_cached_count
#define get_global_idle_state_count fixture_global_count
#define get_idle_state_name fixture_state_name
#define get_idle_states_array fixture_states_array
#define set_effective_cpu_count fixture_effective_count
#define cpu_sysfs_path fixture_cpu_path
#define opendir fixture_opendir
#define sysfs_read_str fixture_read_name
#define sysfs_counter_read fixture_counter_read
#include "../src/platform/cpuidle.c"
#undef get_idle_state_count
#undef init_cpuidle
#undef close_cpuidle
#undef enable_cpuidle
#undef is_cpuidle_enabled
#undef cpuidle_residency_percent
#undef cpuidle_usage_per_sec
#undef update_idle_states
#undef refresh_idle_state_disable_cache_budgeted
#undef configure_cpuidle_fd_cache
#undef get_cpuidle_cached_fd_count
#undef get_global_idle_state_count
#undef get_idle_state_name
#undef get_idle_states_array
#undef set_effective_cpu_count
#undef cpu_sysfs_path
#undef opendir
#undef sysfs_read_str
#undef sysfs_counter_read

static void write_source(int cpu, int state, const char *name, const char *value)
{
	char path[256];
	char sub[64];
	FILE *file;

	snprintf(sub, sizeof(sub), "cpuidle/state%d/%s", state, name);
	assert(fixture_cpu_path(cpu, sub, path, sizeof(path)) == 0);
	file = fopen(path, "w");
	assert(file != NULL);
	assert(fputs(value, file) >= 0);
	assert(fclose(file) == 0);
}

static void setup_tree(void)
{
	char pattern[] = "/tmp/armstat-idle-cache-XXXXXX";
	struct cpu_inventory_seed cpus[] = {
		{ .cpu_id = 0, .present = 1, .online = 1, .core_id = 0 },
		{ .cpu_id = 4, .present = 1, .online = 1, .core_id = 4 },
	};
	char *created = mkdtemp(pattern);

	assert(created != NULL);
	snprintf(tree, sizeof(tree), "%s", created);
	assert(set_cpu_inventory_filter(NULL) == 0);
	assert(cpu_inventory_seed(cpus, 2) == 0);
	for (int cpu_idx = 0; cpu_idx < 2; cpu_idx++) {
		int cpu = cpus[cpu_idx].cpu_id;
		char path[256];
		char sub[64];

		assert(fixture_cpu_path(cpu, "", path, sizeof(path)) == 0);
		assert(mkdir(path, 0700) == 0);
		assert(fixture_cpu_path(cpu, "cpuidle", path, sizeof(path)) == 0);
		assert(mkdir(path, 0700) == 0);
		for (int state = 0; state < MAX_IDLE_STATES; state++) {
			snprintf(sub, sizeof(sub), "cpuidle/state%d", state);
			assert(fixture_cpu_path(cpu, sub, path, sizeof(path)) == 0);
			assert(mkdir(path, 0700) == 0);
			write_source(cpu, state, "name", "LPI\n");
			write_source(cpu, state, "time", "1000\n");
			write_source(cpu, state, "usage", "10\n");
			write_source(cpu, state, "disable", "0\n");
		}
	}
}

static void cleanup_tree(void)
{
	const char *files[] = { "name", "time", "usage", "disable" };

	fixture_close_cpuidle();
	for (int cpu = 0; cpu <= 4; cpu += 4) {
		char path[256];
		char sub[64];

		for (int state = 0; state < MAX_IDLE_STATES; state++) {
			for (int file = 0; file < 4; file++) {
				snprintf(sub, sizeof(sub), "cpuidle/state%d/%s", state, files[file]);
				assert(fixture_cpu_path(cpu, sub, path, sizeof(path)) == 0);
				assert(unlink(path) == 0);
			}
			snprintf(sub, sizeof(sub), "cpuidle/state%d", state);
			assert(fixture_cpu_path(cpu, sub, path, sizeof(path)) == 0);
			assert(rmdir(path) == 0);
		}
		assert(fixture_cpu_path(cpu, "cpuidle", path, sizeof(path)) == 0);
		assert(rmdir(path) == 0);
		assert(fixture_cpu_path(cpu, "", path, sizeof(path)) == 0);
		assert(rmdir(path) == 0);
	}
	assert(rmdir(tree) == 0);
	cleanup_cpu_inventory();
}

static void test_state_count_probed_once_and_all_counters_cached(void)
{
	struct idle_state **states;
	unsigned int mask = (1U << MAX_IDLE_STATES) - 1U;

	state_directory_probes = 0;
	assert(fixture_init_cpuidle() == 0);
	assert(state_directory_probes == 2);
	assert(fixture_global_count() == MAX_IDLE_STATES);
	assert(strcmp(fixture_state_name(15), "LPI") == 0);
	assert(fixture_init_cpuidle() == 0);
	assert(state_directory_probes == 2);
	fixture_configure_cache(64, mask, mask);
	fixture_update_states(1000000, mask, mask);
	states = fixture_states_array();
	assert(fixture_cached_count() == 64);
	for (int cpu = 0; cpu < 2; cpu++) {
		for (int state = 0; state < MAX_IDLE_STATES; state++) {
			int idx = state_fd_index(cpu, state);

			assert(states[cpu][state].available);
			assert(states[cpu][state].time_valid && states[cpu][state].usage_valid);
			assert(isnan(states[cpu][state].percentage));
			assert(fcntl(state_time_cache[idx].fd, F_GETFD) & FD_CLOEXEC);
			assert(fcntl(state_usage_cache[idx].fd, F_GETFD) & FD_CLOEXEC);
		}
	}
	write_source(0, 15, "time", "0\n");
	write_source(0, 15, "usage", "0\n");
	fixture_update_states(1000000, mask, mask);
	assert(states[0][15].time_valid && states[0][15].time == 0);
	assert(states[0][15].usage_valid && states[0][15].usage == 0);
	assert(isnan(states[0][15].percentage));
	write_source(0, 15, "time", "20\n");
	write_source(0, 15, "usage", "5\n");
	fixture_update_states(1000000, mask, mask);
	assert(fabs(states[0][15].percentage - 0.002) < 0.000001);
	assert(states[0][15].usage_per_sec == 5);
	fixture_configure_cache(4, 1U << 15, 1U << 15);
	assert(fixture_cached_count() == 4);
	fixture_update_states(1000000, 1U << 15, 1U << 15);
	assert(states[0][15].time_valid && states[0][15].usage_valid);
	assert(!states[0][0].time_valid && !states[0][0].usage_valid);
	fixture_close_cpuidle();
	assert(fixture_cached_count() == 0);
}

static void test_missing_time_backoff_recovery_and_cached_read_failure(void)
{
	char path[256];
	struct idle_state **states;
	int previous_opens;

	assert(fixture_cpu_path(0, "cpuidle/state15/time", path, sizeof(path)) == 0);
	assert(unlink(path) == 0);
	assert(fixture_init_cpuidle() == 0);
	fixture_configure_cache(4, 1U << 15, 1U << 15);
	time_15_opens = 0;
	fixture_update_states(1000000, 1U << 15, 1U << 15);
	states = fixture_states_array();
	assert(!states[0][15].time_valid && states[0][15].usage_valid);
	assert(time_15_opens == 1);
	fixture_update_states(1000000, 1U << 15, 1U << 15);
	assert(time_15_opens == 1);
	write_source(0, 15, "time", "12345\n");
	fixture_update_states(1000000, 1U << 15, 1U << 15);
	assert(!states[0][15].time_valid && time_15_opens == 1);
	fixture_now_us += 250000;
	fixture_update_states(1000000, 1U << 15, 1U << 15);
	assert(states[0][15].time_valid && states[0][15].time == 12345);
	assert(time_15_opens == 2 && isnan(states[0][15].percentage));
	previous_opens = time_15_opens;
	write_source(0, 15, "time", "bad\n");
	fixture_update_states(1000000, 1U << 15, 1U << 15);
	assert(!states[0][15].time_valid && states[0][15].usage_valid);
	assert(time_15_opens == previous_opens);
	write_source(0, 15, "time", "789\n");
	fixture_update_states(1000000, 1U << 15, 1U << 15);
	assert(states[0][15].time_valid && states[0][15].time == 789);
	assert(time_15_opens == previous_opens + 1);
	fixture_close_cpuidle();
}

int main(void)
{
	setup_tree();
	test_state_count_probed_once_and_all_counters_cached();
	test_missing_time_backoff_recovery_and_cached_read_failure();
	cleanup_tree();
	puts("cpuidle cache tests: 2 cases passed");
	return 0;
}
