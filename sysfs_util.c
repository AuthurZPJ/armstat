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

#include "sysfs_util.h"

int sysfs_read_int_checked(const char *path, int *out)
{
	FILE *fp;

	if (!path || !out)
		return -1;

	*out = 0;
	fp = fopen(path, "r");
	if (!fp)
		return -1;

	if (fscanf(fp, "%d", out) != 1) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return 0;
}

int sysfs_read_ull_checked(const char *path, unsigned long long *out)
{
	FILE *fp;

	if (!path || !out)
		return -1;

	*out = 0;
	fp = fopen(path, "r");
	if (!fp)
		return -1;

	if (fscanf(fp, "%llu", out) != 1) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
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

int fd_read_ull_checked(int fd, unsigned long long *out)
{
	char buf[64];
	ssize_t n;

	if (!out)
		return -1;
	*out = 0;

	if (fd < 0)
		return -1;

	lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return -1;

	buf[n] = '\0';
	if (buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	*out = strtoull(buf, NULL, 10);
	return 0;
}
