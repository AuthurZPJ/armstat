/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM CPU frequency monitoring
 *
 * Two cached-FD strategies:
 *   1. cur_freq_fds[]  — per-CPU cached fd for scaling_cur_freq (hot path)
 *   2. uncore_freq_fd  — single persistent fd for devfreq/cur_freq (hot path)
 *
 * Slow-layer reads (min/max freq, governor, boost) use plain fopen/fclose.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>

#include "cpufreq.h"
#include "collector.h"
#include "cpu_inventory.h"

static int cpu_count;
static int *cur_freq_fds;
static int cur_freq_fd_open_count;

/*
 * Sysfs component names can legitimately be much longer than the handful of
 * characters we usually see in practice (for example devfreq device names).
 * Keep path buffers comfortably above NAME_MAX-derived worst cases so glibc's
 * fortified snprintf checks do not warn about truncation.
 */
#define CPUFREQ_SYSFS_PATH_LEN 512
#define CPUFREQ_NAME_LEN 256
#define MAX_CUR_FREQ_FDS 16

static char uncore_freq_path[CPUFREQ_SYSFS_PATH_LEN];
static char uncore_freq_device[CPUFREQ_NAME_LEN];
static int uncore_freq_fd = -1;
static int uncore_freq_available;

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

/*
 * read_sysfs_uint_slow — slow-layer sysfs uint reader via fopen/fclose.
 * Used for min/max freq, boost (refreshed every ~5s, not on the hot path).
 */
static unsigned int read_sysfs_uint_slow(const char *path)
{
	FILE *fp;
	unsigned int value = 0;

	fp = fopen(path, "r");
	if (fp) {
		if (fscanf(fp, "%u", &value) != 1)
			value = 0;
		fclose(fp);
	}
	return value;
}

/*
 * read_sysfs_file — read a sysfs string value via fopen/fclose.
 */
static char *read_sysfs_file(const char *path, char *buf, size_t len)
{
	FILE *fp;

	if (!buf || len == 0)
		return NULL;

	fp = fopen(path, "r");
	if (!fp)
		return NULL;

	if (fgets(buf, len, fp))
		buf[strcspn(buf, "\n")] = 0;
	else
		buf[0] = 0;

	fclose(fp);
	return buf;
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
		if (access(path, R_OK) != 0)
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
	unsigned int freq_khz = 0;
	char buf[32];
	int cpu_id;
	ssize_t n;

	if (tracked_idx < 0 || tracked_idx >= cpu_count)
		return -1;

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (cpu_id < 0)
		return -1;

	if (cur_freq_fds) {
		int fd = cur_freq_fds[tracked_idx];

		if (fd < 0) {
			cpu_sysfs_path(cpu_id, "cpufreq/scaling_cur_freq",
				       path, sizeof(path));
			if (cur_freq_fd_open_count < MAX_CUR_FREQ_FDS)
				fd = open(path, O_RDONLY);
			if (fd >= 0) {
				cur_freq_fds[tracked_idx] = fd;
				cur_freq_fd_open_count++;
			}
		}

		if (fd >= 0) {
			lseek(fd, 0, SEEK_SET);
			n = read(fd, buf, sizeof(buf) - 1);
			if (n > 0) {
				buf[n] = '\0';
				if (n > 0 && buf[n - 1] == '\n')
					buf[n - 1] = '\0';
				freq_khz = (unsigned int)atoi(buf);
			}
		}
	}

	if (freq_khz == 0) {
		cpu_sysfs_path(cpu_id, "cpufreq/scaling_cur_freq",
			       path, sizeof(path));
		freq_khz = read_sysfs_uint_slow(path);
	}
	if (freq_khz == 0)
		return -1;

	*freq = freq_khz;
	return 0;
}

int read_cpu_min_max_freq(int tracked_idx, unsigned int *min, unsigned int *max)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];
	int cpu_id;

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (cpu_id < 0)
		return -1;

	cpu_sysfs_path(cpu_id, "cpufreq/scaling_min_freq", path, sizeof(path));
	*min = read_sysfs_uint_slow(path);

	cpu_sysfs_path(cpu_id, "cpufreq/scaling_max_freq", path, sizeof(path));
	*max = read_sysfs_uint_slow(path);

	return 0;
}

int read_cpu_governor(int tracked_idx, char *governor, size_t len)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];
	char gov_buf[32];
	char *gov;
	int cpu_id;

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (cpu_id < 0)
		return -1;

	cpu_sysfs_path(cpu_id, "cpufreq/scaling_governor", path, sizeof(path));

	gov = read_sysfs_file(path, gov_buf, sizeof(gov_buf));
	if (!gov)
		return -1;

	copy_cstring(governor, len, gov);
	return 0;
}

/*
 * read_sysfs_uint_exists — check whether a sysfs uint file exists and is readable.
 */
static int read_sysfs_uint_exists(const char *path, unsigned int *value)
{
	FILE *fp = fopen(path, "r");

	if (!fp)
		return -1;

	if (fscanf(fp, "%u", value) != 1)
		*value = 0;
	fclose(fp);
	return 0;
}

int read_cpu_boost(int tracked_idx, int *boost)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];
	unsigned int value = 0;
	int cpu_id;

	cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);
	if (!boost || cpu_id < 0)
		return -1;

	/* Try per-CPU boost file first */
	cpu_sysfs_path(cpu_id, "cpufreq/boost", path, sizeof(path));
	if (read_sysfs_uint_exists(path, &value) == 0) {
		*boost = (int)value;
		return 0;
	}

	/* Fall back to global boost file */
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpufreq/boost");
	if (read_sysfs_uint_exists(path, &value) == 0) {
		*boost = (int)value;
		return 0;
	}

	return -1;
}

int read_uncore_freq(unsigned long long *freq_hz)
{
	char buf[64];
	ssize_t n;
	unsigned long long value = 0;

	if (!freq_hz || !uncore_freq_available || uncore_freq_path[0] == '\0')
		return -1;

	if (uncore_freq_fd < 0)
		uncore_freq_fd = open(uncore_freq_path, O_RDONLY);

	if (uncore_freq_fd >= 0) {
		lseek(uncore_freq_fd, 0, SEEK_SET);
		n = read(uncore_freq_fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			if (n > 0 && buf[n - 1] == '\n')
				buf[n - 1] = '\0';
			value = strtoull(buf, NULL, 10);
		}
	}

	if (value == 0) {
		FILE *fp = fopen(uncore_freq_path, "r");
		if (fp) {
			if (fscanf(fp, "%llu", &value) != 1)
				value = 0;
			fclose(fp);
		}
	}
	if (value == 0)
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

	cur_freq_fds = calloc(cpu_count, sizeof(int));
	if (!cur_freq_fds)
		return -1;

	cur_freq_fd_open_count = 0;
	for (int i = 0; i < cpu_count; i++)
		cur_freq_fds[i] = -1;

	uncore_freq_path[0] = '\0';
	uncore_freq_device[0] = '\0';
	uncore_freq_available = 0;
	uncore_freq_fd = -1;
	discover_uncore_freq_source();

	return 0;
}

void close_cpufreq(void)
{
	if (cur_freq_fds) {
		for (int i = 0; i < cpu_count; i++) {
			if (cur_freq_fds[i] >= 0)
				close(cur_freq_fds[i]);
		}
		free(cur_freq_fds);
		cur_freq_fds = NULL;
	}
	cur_freq_fd_open_count = 0;

	if (uncore_freq_fd >= 0) {
		close(uncore_freq_fd);
		uncore_freq_fd = -1;
	}
	uncore_freq_available = 0;
	uncore_freq_path[0] = '\0';
	uncore_freq_device[0] = '\0';
}
