/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM CPU frequency monitoring
 *
 * Current-frequency fds share a runtime budget with cpuidle counters; a single
 * devfreq source may also retain its fd. Per-CPU readings remain independent.
 *
 * Selected min/max and boost configuration reads use ephemeral descriptors.
 * Unavailable numeric sources use bounded retry backoff. Governor is a slow
 * string read. The one global boost fallback is read once per sampling cycle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>

#include "cpufreq.h"
#include "collector.h"
#include "cpu_inventory.h"
#include "sysfs_util.h"

static int cpu_count;
static struct sysfs_counter_cache *cur_freq_cache;
static struct sysfs_counter_cache *min_freq_cache;
static struct sysfs_counter_cache *max_freq_cache;
static struct sysfs_counter_cache *boost_cache;
static int cur_freq_fd_open_count;
static int cur_freq_fd_cap;

/*
 * Sysfs component names can legitimately be much longer than the handful of
 * characters we usually see in practice (for example devfreq device names).
 * Keep path buffers comfortably above NAME_MAX-derived worst cases so glibc's
 * fortified snprintf checks do not warn about truncation.
 */
#define CPUFREQ_SYSFS_PATH_LEN 512
#define CPUFREQ_NAME_LEN 256

static char uncore_freq_path[CPUFREQ_SYSFS_PATH_LEN];
static char uncore_freq_device[CPUFREQ_NAME_LEN];
static struct sysfs_counter_cache uncore_freq_cache = { .fd = -1 };
static struct sysfs_counter_cache global_boost_cache = { .fd = -1 };
static int uncore_freq_fd_count;
static int uncore_freq_available;
static int global_boost_read;
static int global_boost_status;
static unsigned long long global_boost_value;

void cpufreq_begin_sample(void)
{
	global_boost_read = 0;
}

void configure_cpufreq_fd_cache(int fd_cap, int collect_current)
{
	cur_freq_fd_cap = fd_cap > 0 && collect_current ? fd_cap : 0;
	if (!cur_freq_cache)
		return;
	for (int i = cpu_count - 1; i >= 0 &&
	     cur_freq_fd_open_count > cur_freq_fd_cap; i--)
		sysfs_counter_close(&cur_freq_cache[i], &cur_freq_fd_open_count);
}

int get_cpufreq_cached_fd_count(void)
{
	return cur_freq_fd_open_count + uncore_freq_fd_count;
}

static void lowercase_cstring(char *dst, size_t dst_size, const char *src)
{
	size_t i;

	if (!dst || dst_size == 0)
		return;

	if (!src) {
		dst[0] = '\0';
		return;
	}

	for (i = 0; i + 1 < dst_size && src[i] != '\0'; i++)
		dst[i] = (char)tolower((unsigned char)src[i]);
	dst[i] = '\0';
}

static int devfreq_name_looks_like_uncore(const char *name)
{
	static const char *const keywords[] = {
		"uncore",
		"interconnect",
		"fabric",
		"noc",
		"cci",
		"ccn",
		"cmn",
	};
	char lower_name[CPUFREQ_NAME_LEN];

	if (!name || !*name)
		return 0;

	lowercase_cstring(lower_name, sizeof(lower_name), name);
	for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
		if (strstr(lower_name, keywords[i]))
			return 1;
	}

	return 0;
}

static int discover_uncore_freq_source(void)
{
	DIR *dir;
	struct dirent *entry;
	char matched_device[CPUFREQ_NAME_LEN] = "";
	char path[CPUFREQ_SYSFS_PATH_LEN];
	int matched_count = 0;

	dir = opendir("/sys/class/devfreq");
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path),
			 "/sys/class/devfreq/%s/cur_freq",
			 entry->d_name);
		if (!sysfs_path_exists(path))
			continue;

		if (!devfreq_name_looks_like_uncore(entry->d_name))
			continue;

		matched_count++;
		if (matched_count == 1)
			snprintf(matched_device, sizeof(matched_device), "%s",
				 entry->d_name);
	}

	closedir(dir);
	if (matched_count != 1 || matched_device[0] == '\0')
		return -1;

	copy_cstring(uncore_freq_device, sizeof(uncore_freq_device), matched_device);
	snprintf(uncore_freq_path, sizeof(uncore_freq_path),
		 "/sys/class/devfreq/%s/cur_freq", matched_device);
	uncore_freq_available = 1;
	return 0;
}

int read_cpu_freq(int tracked_idx, unsigned int *freq)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];
	const char *source_path = NULL;
	unsigned long long freq_raw = 0;
	int cpu_id;

	if (!freq || tracked_idx < 0 || tracked_idx >= cpu_count)
		return -1;

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (cpu_id < 0)
		return -1;

	if (cur_freq_cache && cur_freq_cache[tracked_idx].fd < 0) {
		cpu_sysfs_path(cpu_id, "cpufreq/cpuinfo_cur_freq", path, sizeof(path));
		source_path = path;
	}
	if (!cur_freq_cache ||
	    sysfs_counter_read(&cur_freq_cache[tracked_idx], source_path, &freq_raw,
			       &cur_freq_fd_open_count, cur_freq_fd_cap) < 0 ||
	    freq_raw == 0 || freq_raw > UINT_MAX)
		return -1;

	*freq = (unsigned int)freq_raw;
	return 0;
}

int read_cpu_min_max_freq_checked(int tracked_idx,
				  unsigned int *min, int *min_valid,
				  unsigned int *max, int *max_valid)
{
	return read_cpu_min_max_freq_selected(tracked_idx, min, min_valid,
					     max, max_valid, 1, 1);
}

int read_cpu_min_max_freq_selected(int tracked_idx,
				 unsigned int *min, int *min_valid,
				 unsigned int *max, int *max_valid,
				 int read_min, int read_max)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];
	unsigned long long raw = 0;
	int cpu_id;

	if (!min || !min_valid || !max || !max_valid)
		return -1;
	*min = 0;
	*max = 0;
	*min_valid = 0;
	*max_valid = 0;

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (cpu_id < 0 || tracked_idx < 0 || tracked_idx >= cpu_count)
		return -1;

	cpu_sysfs_path(cpu_id, "cpufreq/scaling_min_freq", path, sizeof(path));
	if (read_min && min_freq_cache &&
	    sysfs_counter_read(&min_freq_cache[tracked_idx], path, &raw,
			       NULL, 0) == 0 && raw <= UINT_MAX) {
		*min = (unsigned int)raw;
		*min_valid = 1;
	}

	cpu_sysfs_path(cpu_id, "cpufreq/scaling_max_freq", path, sizeof(path));
	if (read_max && max_freq_cache &&
	    sysfs_counter_read(&max_freq_cache[tracked_idx], path, &raw,
			       NULL, 0) == 0 && raw <= UINT_MAX) {
		*max = (unsigned int)raw;
		*max_valid = 1;
	}

	return *min_valid || *max_valid ? 0 : -1;
}

int read_cpu_governor(int tracked_idx, char *governor, size_t len)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];
	char gov_buf[32];
	char *gov;
	int cpu_id;

	if (!governor || len == 0)
		return -1;

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (cpu_id < 0)
		return -1;

	cpu_sysfs_path(cpu_id, "cpufreq/scaling_governor", path, sizeof(path));

	gov = sysfs_read_str(path, gov_buf, sizeof(gov_buf));
	if (!*gov)
		return -1;

	copy_cstring(governor, len, gov);
	return 0;
}

int read_cpu_boost(int tracked_idx, int *boost)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];
	unsigned long long value = 0;
	int cpu_id;

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (!boost || cpu_id < 0 || tracked_idx < 0 || tracked_idx >= cpu_count)
		return -1;

	/* Try per-CPU boost file first */
	cpu_sysfs_path(cpu_id, "cpufreq/boost", path, sizeof(path));
	if (boost_cache &&
	    sysfs_counter_read(&boost_cache[tracked_idx], path, &value,
			       NULL, 0) == 0 && value <= 1) {
		*boost = (int)value;
		return 0;
	}

	/* Fall back to global boost file */
	if (!global_boost_read || !sysfs_counter_sample_active()) {
		global_boost_read = 1;
		global_boost_status = sysfs_counter_read(&global_boost_cache,
			"/sys/devices/system/cpu/cpufreq/boost",
			&global_boost_value, NULL, 0);
	}
	if (global_boost_status == 0 && global_boost_value <= 1) {
		*boost = (int)global_boost_value;
		return 0;
	}

	return -1;
}

int read_uncore_freq(unsigned long long *freq_hz)
{
	unsigned long long value = 0;

	if (!freq_hz || !uncore_freq_available || uncore_freq_path[0] == '\0')
		return -1;

	if (sysfs_counter_read(&uncore_freq_cache, uncore_freq_path, &value,
			       &uncore_freq_fd_count, 1) < 0 || value == 0)
		return -1;

	*freq_hz = value;
	return 0;
}

int has_uncore_freq_support(void)
{
	return uncore_freq_available;
}

const char *get_uncore_freq_device_name(void)
{
	return uncore_freq_available ? uncore_freq_device : NULL;
}

int init_cpufreq(void)
{
	cpu_count = get_tracked_cpu_count();
	if (cpu_count <= 0)
		return -1;

	cur_freq_cache = calloc(cpu_count, sizeof(*cur_freq_cache));
	min_freq_cache = calloc(cpu_count, sizeof(*min_freq_cache));
	max_freq_cache = calloc(cpu_count, sizeof(*max_freq_cache));
	boost_cache = calloc(cpu_count, sizeof(*boost_cache));
	if (!cur_freq_cache || !min_freq_cache || !max_freq_cache || !boost_cache) {
		/* calloc fds are not initialized yet; free without closing fd 0. */
		free(cur_freq_cache);
		free(min_freq_cache);
		free(max_freq_cache);
		free(boost_cache);
		cur_freq_cache = min_freq_cache = max_freq_cache = boost_cache = NULL;
		cpu_count = 0;
		return -1;
	}

	cur_freq_fd_open_count = 0;
	cur_freq_fd_cap = 0;
	for (int i = 0; i < cpu_count; i++) {
		sysfs_counter_init(&cur_freq_cache[i]);
		sysfs_counter_init(&min_freq_cache[i]);
		sysfs_counter_init(&max_freq_cache[i]);
		sysfs_counter_init(&boost_cache[i]);
	}

	uncore_freq_path[0] = '\0';
	uncore_freq_device[0] = '\0';
	uncore_freq_available = 0;
	sysfs_counter_init(&uncore_freq_cache);
	sysfs_counter_init(&global_boost_cache);
	uncore_freq_fd_count = 0;
	cpufreq_begin_sample();
	discover_uncore_freq_source();

	return 0;
}

int get_cpufreq_tracked_count(void)
{
	return cpu_count;
}

void close_cpufreq(void)
{
	if (cur_freq_cache) {
		for (int i = 0; i < cpu_count; i++) {
			sysfs_counter_close(&cur_freq_cache[i], &cur_freq_fd_open_count);
		}
		free(cur_freq_cache);
		cur_freq_cache = NULL;
	}
	free(min_freq_cache);
	free(max_freq_cache);
	free(boost_cache);
	min_freq_cache = max_freq_cache = boost_cache = NULL;
	cur_freq_fd_open_count = 0;
	cur_freq_fd_cap = 0;

	sysfs_counter_close(&uncore_freq_cache, &uncore_freq_fd_count);
	sysfs_counter_close(&global_boost_cache, NULL);
	cpufreq_begin_sample();
	uncore_freq_available = 0;
	uncore_freq_path[0] = '\0';
	uncore_freq_device[0] = '\0';
	cpu_count = 0;
}
