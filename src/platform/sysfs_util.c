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

#include "sysfs_util.h"

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

	if (!path)
		return buf;

	fp = fopen(path, "r");
	if (!fp)
		return buf;

	if (fgets(buf, (int)len, fp))
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
