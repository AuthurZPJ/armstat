/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sysfs_util.c - Shared sysfs/procfs/fd read primitives
 *
 * See sysfs_util.h for the interface contract.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <time.h>
#include <dirent.h>
#include <sys/resource.h>

#include "sysfs_util.h"

#define SYSFS_FD_RESERVE 64
#define SYSFS_RETRY_INITIAL_US 250000ULL
#define SYSFS_RETRY_MAX_US 5000000ULL

static uint64_t sample_time_us;

static uint64_t counter_time_us(void)
{
	struct timespec now;

	if (sample_time_us)
		return sample_time_us;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000000ULL + now.tv_nsec / 1000;
}

void sysfs_counter_begin_sample(void)
{
	sample_time_us = 0;
	sample_time_us = counter_time_us();
}

void sysfs_counter_end_sample(void)
{
	sample_time_us = 0;
}

int sysfs_counter_sample_active(void)
{
	return sample_time_us != 0;
}

void sysfs_counter_init(struct sysfs_counter_cache *cache)
{
	memset(cache, 0, sizeof(*cache));
	cache->fd = -1;
}

static void counter_drop_fd(struct sysfs_counter_cache *cache, int *open_count)
{
	if (cache->fd >= 0) {
		close(cache->fd);
		cache->fd = -1;
		if (open_count && *open_count > 0)
			(*open_count)--;
	}
}

void sysfs_counter_close(struct sysfs_counter_cache *cache, int *open_count)
{
	counter_drop_fd(cache, open_count);
	sysfs_counter_init(cache);
}

static void counter_record_failure(struct sysfs_counter_cache *cache,
				    uint64_t now, int error)
{
	uint64_t delay;

	/* Transient errors and malformed values are retried at the next sample. */
	if (error != ENOENT && error != ENODEV && error != ENXIO &&
	    error != EACCES && error != EPERM && error != ESTALE)
		return;
	if (cache->failures < 6)
		cache->failures++;
	delay = SYSFS_RETRY_INITIAL_US << (cache->failures - 1);
	if (delay > SYSFS_RETRY_MAX_US)
		delay = SYSFS_RETRY_MAX_US;
	cache->retry_after_us = UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
}

int sysfs_counter_read(struct sysfs_counter_cache *cache, const char *path,
			unsigned long long *out, int *open_count, int fd_cap)
{
	uint64_t now = counter_time_us();
	int fd;
	int cached;
	int error;

	if (!cache || !out || (!path && cache->fd < 0))
		return -1;
	*out = 0;
	if (now && now < cache->retry_after_us)
		return -1;
	fd = cache->fd;
	cached = fd >= 0;
	if (fd < 0) {
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			counter_record_failure(cache, now, errno);
			return -1;
		}
	}
	errno = 0;
	if (fd_read_ull_checked(fd, out) < 0) {
		error = errno;
		if (cached)
			counter_drop_fd(cache, open_count);
		else
			close(fd);
		counter_record_failure(cache, now, error);
		return -1;
	}
	cache->failures = 0;
	cache->retry_after_us = 0;
	if (!cached) {
		if (open_count && *open_count < fd_cap) {
			cache->fd = fd;
			(*open_count)++;
		} else {
			close(fd);
		}
	}
	return 0;
}

int sysfs_counter_fd_budget(int requested, int cached_fds)
{
	struct rlimit limit;
	struct dirent *entry;
	DIR *dir;
	unsigned long long used = 0;
	unsigned long long available;

	if (requested <= 0 || getrlimit(RLIMIT_NOFILE, &limit) < 0)
		return 0;
	if (limit.rlim_cur == RLIM_INFINITY)
		return requested;
	dir = opendir("/proc/self/fd");
	if (!dir)
		dir = opendir("/dev/fd");
	if (!dir)
		return 0;
	while ((entry = readdir(dir)) != NULL) {
		char *end;
		long fd;

		if (entry->d_name[0] == '.')
			continue;
		errno = 0;
		fd = strtol(entry->d_name, &end, 10);
		if (!errno && end != entry->d_name && !*end &&
		    fd >= 0 && fd != dirfd(dir))
			used++;
	}
	closedir(dir);
	if (cached_fds > 0 && (unsigned long long)cached_fds <= used)
		used -= cached_fds;
	if ((unsigned long long)limit.rlim_cur <= used + SYSFS_FD_RESERVE)
		return 0;
	available = (unsigned long long)limit.rlim_cur - used - SYSFS_FD_RESERVE;
	return available < (unsigned long long)requested ? (int)available : requested;
}

static int parse_int_value(const char *text, int *out)
{
	char *end;
	long value;

	if (!text || !out)
		return -1;

	errno = 0;
	value = strtol(text, &end, 10);
	if (end == text || errno == ERANGE || value < INT_MIN || value > INT_MAX)
		return -1;
	while (isspace((unsigned char)*end))
		end++;
	if (*end != '\0')
		return -1;

	*out = (int)value;
	return 0;
}

static int parse_ull_value(const char *text, unsigned long long *out)
{
	const char *start = text;
	char *end;
	unsigned long long value;

	if (!text || !out)
		return -1;
	while (isspace((unsigned char)*start))
		start++;
	if (*start == '-')
		return -1;

	errno = 0;
	value = strtoull(start, &end, 10);
	if (end == start || errno == ERANGE)
		return -1;
	while (isspace((unsigned char)*end))
		end++;
	if (*end != '\0')
		return -1;

	*out = value;
	return 0;
}

static int input_line_complete(FILE *fp, const char *buf)
{
	int ch;

	if (strchr(buf, '\n'))
		return 1;

	ch = fgetc(fp);
	return ch == EOF && !ferror(fp);
}

int sysfs_read_int_checked(const char *path, int *out)
{
	FILE *fp;
	char buf[128];
	int value;

	if (!path || !out)
		return -1;

	*out = 0;
	fp = fopen(path, "r");
	if (!fp)
		return -1;

	if (!fgets(buf, sizeof(buf), fp) || !input_line_complete(fp, buf) ||
	    parse_int_value(buf, &value) < 0) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	*out = value;
	return 0;
}

int sysfs_read_ull_checked(const char *path, unsigned long long *out)
{
	FILE *fp;
	char buf[128];
	unsigned long long value;

	if (!path || !out)
		return -1;

	*out = 0;
	fp = fopen(path, "r");
	if (!fp)
		return -1;

	if (!fgets(buf, sizeof(buf), fp) || !input_line_complete(fp, buf) ||
	    parse_ull_value(buf, &value) < 0) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	*out = value;
	return 0;
}

char *sysfs_read_str(const char *path, char *buf, size_t len)
{
	FILE *fp;

	if (!buf || len == 0)
		return buf;
	buf[0] = '\0';

	if (!path || len < 2 || len > INT_MAX)
		return buf;

	fp = fopen(path, "r");
	if (!fp)
		return buf;

	if (fgets(buf, (int)len, fp) && input_line_complete(fp, buf) &&
	    !ferror(fp))
		buf[strcspn(buf, "\n")] = '\0';
	else
		buf[0] = '\0';

	fclose(fp);
	return buf;
}

int sysfs_path_exists(const char *path)
{
	if (!path)
		return 0;
	return access(path, R_OK) == 0 ? 1 : 0;
}

static int fd_read_text_checked(int fd, char *buf, size_t len)
{
	off_t offset;
	ssize_t n;
	ssize_t text_len;
	char extra;

	if (fd < 0 || !buf || len < 2)
		return -1;

	do {
		offset = lseek(fd, 0, SEEK_SET);
	} while (offset < 0 && errno == EINTR);
	if (offset < 0)
		return -1;

	do {
		n = read(fd, buf, len - 1);
	} while (n < 0 && errno == EINTR);
	if (n <= 0)
		return -1;
	text_len = n;
	if ((size_t)n == len - 1) {
		do {
			n = read(fd, &extra, 1);
		} while (n < 0 && errno == EINTR);
		if (n != 0)
			return -1;
	}

	buf[text_len] = '\0';
	return 0;
}

int fd_read_ull_checked(int fd, unsigned long long *out)
{
	char buf[64];

	if (!out)
		return -1;
	*out = 0;
	if (fd_read_text_checked(fd, buf, sizeof(buf)) < 0)
		return -1;
	return parse_ull_value(buf, out);
}

int fd_read_int_checked(int fd, int *out)
{
	char buf[64];

	if (!out)
		return -1;
	*out = 0;
	if (fd_read_text_checked(fd, buf, sizeof(buf)) < 0)
		return -1;
	return parse_int_value(buf, out);
}
