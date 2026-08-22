/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sysfs_util.h - Shared sysfs/procfs/fd read primitives
 *
 * Consolidates the duplicated fopen/fscanf/fclose and lseek/read/convert
 * patterns that were previously copy-pasted across topology.c,
 * power_sensor.c, cpufreq.c, and cpuidle.c with subtly different error
 * conventions.
 *
 * Design decisions:
 *   - All numeric readers use the "checked" convention (return 0 on success,
 *     -1 on failure, value via out-param). This eliminates the ambiguity
 *     where some callers used -1 as an error sentinel and others used 0.
 *   - sysfs_read_str always returns buf (with buf[0]=0 on failure) so
 *     callers that forget to check get an empty string, not a NULL deref.
 *   - Numeric readers reject overflow, negative unsigned values, empty input,
 *     and non-whitespace trailing data instead of accepting partial values.
 *   - fd_read_ull_checked uses a 64-byte buffer and requires both seek and
 *     read to succeed. Callers needing int or long long results cast only
 *     after checking their destination range.
 */

#ifndef ARMSTAT_SYSFS_UTIL_H
#define ARMSTAT_SYSFS_UTIL_H

#include <stddef.h>

/*
 * Read a single int from a sysfs/procfs file.
 * Returns 0 on success (*out set), -1 on open/read/parse/range failure.
 */
int sysfs_read_int_checked(const char *path, int *out);

/*
 * Read a single unsigned long long from a sysfs/procfs file.
 * Also serves long long callers via cast.
 * Returns 0 on success (*out set), -1 on open/read/parse/range failure.
 */
int sysfs_read_ull_checked(const char *path, unsigned long long *out);

/*
 * Read a string (first line) from a sysfs/procfs file into buf.
 * Trailing newline is stripped. Always returns buf; on failure buf[0]=0.
 */
char *sysfs_read_str(const char *path, char *buf, size_t len);

/*
 * Check if a path is readable.
 * Returns 1 if accessible, 0 otherwise.
 */
int sysfs_path_exists(const char *path);

/*
 * Read a single unsigned long long from an already-open file descriptor.
 * Does lseek(fd, 0, SEEK_SET) before reading. Uses a 64-byte stack buffer.
 * Returns 0 on success (*out set), -1 on seek/read/parse/range failure.
 */
int fd_read_ull_checked(int fd, unsigned long long *out);

/* Signed-int variant for already-open files such as thermal-zone readings. */
int fd_read_int_checked(int fd, int *out);

#endif /* ARMSTAT_SYSFS_UTIL_H */
