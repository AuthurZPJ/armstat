/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM CPU frequency monitoring
 *
 * Cur/min/max frequency reads are hot-path operations and use cached file
 * descriptors. Governor reads stay on a simpler buffered path because they
 * live in the slow-changing layer.
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

/*
 * Global state
 */
static int cpu_count;
static int *cur_freq_fds;
static int cur_freq_fd_cap;
static int cur_freq_fd_open_count;
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
static int uncore_freq_fd = -1;
static int uncore_freq_available;

/*
 * File descriptor cache for sysfs
 * Reduces overhead of repeated open/close operations
 */
/*
 * PMU can hold one group leader plus member fds per tracked CPU. Keep
 * cpufreq's persistent caches deliberately small so enabling PMU does not
 * push the process over conservative RLIMIT_NOFILE values on large servers.
 */
#define MAX_CACHED_FDS	8
#define MAX_CUR_FREQ_FDS 16
struct cached_fd {
	char path[CPUFREQ_SYSFS_PATH_LEN];
	int fd;
	int valid;
};

static struct cached_fd fd_cache[MAX_CACHED_FDS];
static int fd_cache_count = 0;

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
 * get_cached_fd - Get cached file descriptor or open new one
 * @path: sysfs file path
 *
 * Returns: file descriptor (>=0) on success, -1 on failure
 *
 * This function maintains a cache of open file descriptors to avoid
 * the overhead of repeated open/close operations for frequently
 * accessed sysfs files.
 */
static int get_cached_fd(const char *path)
{
	int i;

	/* Check if path is already in cache */
	for (i = 0; i < fd_cache_count; i++) {
		if (strcmp(fd_cache[i].path, path) == 0)
			return fd_cache[i].fd;
	}

	/* Not in cache, try to open the file */
	if (fd_cache_count < MAX_CACHED_FDS) {
		int fd = open(path, O_RDONLY);
		if (fd >= 0) {
			copy_cstring(fd_cache[fd_cache_count].path,
				     sizeof(fd_cache[fd_cache_count].path),
				     path);
			fd_cache[fd_cache_count].fd = fd;
			fd_cache[fd_cache_count].valid = 0;
			fd_cache_count++;
			return fd;
		}
	}

	/* Cache full or open failed */
	return -1;
}

/* Optimized sysfs read with cached file descriptors */
static unsigned int read_sysfs_uint(const char *path)
{
	int fd;
	char buf[32];
	ssize_t n;

	fd = get_cached_fd(path);
	if (fd >= 0) {
		/* Seek to beginning and read */
		lseek(fd, 0, SEEK_SET);
		n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			/* Remove trailing newline */
			if (n > 0 && buf[n-1] == '\n')
				buf[n-1] = '\0';
			return (unsigned int)atoi(buf);
		}
		return 0;
	}

	/* Fallback to fopen/fclose */
	FILE *fp = fopen(path, "r");
	unsigned int value = 0;
	if (fp) {
		if (fscanf(fp, "%u", &value) != 1)
			value = 0;
		fclose(fp);
	}
	return value;
}

static unsigned long long read_sysfs_ull_fast(const char *path)
{
	int fd;
	char buf[64];
	ssize_t n;

	fd = get_cached_fd(path);
	if (fd >= 0) {
		lseek(fd, 0, SEEK_SET);
		n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = '\0';
			if (n > 0 && buf[n - 1] == '\n')
				buf[n - 1] = '\0';
			return strtoull(buf, NULL, 10);
		}
		return 0;
	}

	{
		FILE *fp = fopen(path, "r");
		unsigned long long value = 0;

		if (fp) {
			if (fscanf(fp, "%llu", &value) != 1)
				value = 0;
			fclose(fp);
		}
		return value;
	}
}

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

int read_cpu_freq(int cpu, unsigned int *freq)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];
	unsigned int freq_khz = 0;
	char buf[32];
	ssize_t n;

	if (cur_freq_fds && cpu >= 0 && cpu < cpu_count) {
		int fd = cur_freq_fds[cpu];

		if (fd < 0) {
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",
				 cpu);
			if (cur_freq_fd_open_count < cur_freq_fd_cap)
				fd = open(path, O_RDONLY);
			if (fd >= 0) {
				cur_freq_fds[cpu] = fd;
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
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",
			 cpu);
		freq_khz = read_sysfs_uint(path);
	}
	if (freq_khz == 0)
		return -1;

	*freq = freq_khz;
	return 0;
}

int read_cpu_min_max_freq(int cpu, unsigned int *min, unsigned int *max)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq",
		 cpu);
	*min = read_sysfs_uint(path);

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",
		 cpu);
	*max = read_sysfs_uint(path);

	return 0;
}

int read_cpu_governor(int cpu, char *governor, size_t len)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];
	char gov_buf[32];
	char *gov;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor",
		 cpu);

	gov = read_sysfs_file(path, gov_buf, sizeof(gov_buf));
	if (!gov)
		return -1;

	copy_cstring(governor, len, gov);
	return 0;
}

int read_cpu_boost(int cpu, int *boost)
{
	char path[CPUFREQ_SYSFS_PATH_LEN];
	unsigned int value;

	if (!boost)
		return -1;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/cpufreq/boost",
		 cpu);
	value = read_sysfs_uint(path);
	if (value == 0) {
		FILE *fp = fopen(path, "r");
		if (!fp) {
			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpufreq/boost");
			fp = fopen(path, "r");
			if (!fp)
				return -1;
		}
		fclose(fp);
		value = read_sysfs_uint(path);
	}

	*boost = (int)value;
	return 0;
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

	if (value == 0)
		value = read_sysfs_ull_fast(uncore_freq_path);
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
	cpu_count = get_cpu_id_array_size();
	if (cpu_count <= 0)
		return -1;

	cur_freq_fds = calloc(cpu_count, sizeof(int));
	if (!cur_freq_fds) {
		return -1;
	}
	cur_freq_fd_cap = MAX_CUR_FREQ_FDS;
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
	int i;

	/* Close cached file descriptors */
	for (i = 0; i < fd_cache_count; i++) {
		if (fd_cache[i].fd >= 0)
			close(fd_cache[i].fd);
		fd_cache[i].fd = -1;
	}
	fd_cache_count = 0;

	if (cur_freq_fds) {
		for (i = 0; i < cpu_count; i++) {
			if (cur_freq_fds[i] >= 0)
				close(cur_freq_fds[i]);
		}
		free(cur_freq_fds);
		cur_freq_fds = NULL;
	}
	cur_freq_fd_open_count = 0;
	cur_freq_fd_cap = 0;

	if (uncore_freq_fd >= 0) {
		close(uncore_freq_fd);
		uncore_freq_fd = -1;
	}
	uncore_freq_available = 0;
	uncore_freq_path[0] = '\0';
	uncore_freq_device[0] = '\0';
}
