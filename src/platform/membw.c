/* SPDX-License-Identifier: GPL-2.0 */
/*
 * membw.c - Memory bandwidth monitoring
 *
 * Responsibilities:
 *   - Discover memory bandwidth counters
 *   - Calculate bandwidth from deltas
 *
 * Does NOT handle:
 *   - Sensor discovery (power_sensor.c)
 *   - Power/energy calculations (power_interval.c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <math.h>
#include <fcntl.h>

#include "power.h"
#include "power_internal.h"
#include "sysfs_util.h"

/* Memory bandwidth state */
static int mem_bw_support;
static int mem_bw_candidate_count;
static int mem_bw_read_fd = -1;
static char mem_bw_read_path[POWER_SYSFS_PATH_LEN];
static unsigned long long prev_mem_read;
static double interval_mem_bw;
static int mem_bw_initialized;

/* ============================================================================
 * DISCOVERY
 * ============================================================================ */

/*
 * Scan for memory bandwidth counters
 */
static int scan_mem_bw_counters(void)
{
	DIR *dir;
	struct dirent *entry;
	char matched_path[POWER_SYSFS_PATH_LEN] = "";

	mem_bw_read_path[0] = '\0';
	mem_bw_candidate_count = 0;

	/* Look in /sys/class/memory/ */
	dir = opendir("/sys/class/memory");
	if (dir) {
		while ((entry = readdir(dir)) != NULL) {
			/* Look for mem* directories */
			if (strncmp(entry->d_name, "mem", 3) == 0) {
				char path[POWER_SYSFS_PATH_LEN];

				/* Check for mem_bytes_read */
				snprintf(path, sizeof(path),
					 "/sys/class/memory/%s/mem_bytes_read",
					 entry->d_name);
				if (sysfs_path_exists(path)) {
					mem_bw_candidate_count++;
					if (mem_bw_candidate_count == 1)
						snprintf(matched_path,
							 sizeof(matched_path),
							 "%s", path);
				}
			}
		}
		closedir(dir);
	}

	if (mem_bw_candidate_count != 1)
		return 0;

	snprintf(mem_bw_read_path, sizeof(mem_bw_read_path), "%s", matched_path);
	return 1;
}

/*
 * Initialize memory bandwidth monitoring (public)
 */
int init_mem_bw(void)
{
	/* Idempotency: close existing fp before re-initializing */
	if (mem_bw_read_fd >= 0) {
		close(mem_bw_read_fd);
		mem_bw_read_fd = -1;
	}

	/* Scan for memory bandwidth counters */
	mem_bw_support = scan_mem_bw_counters();

	if (mem_bw_support && mem_bw_read_path[0])
		mem_bw_read_fd = open(mem_bw_read_path, O_RDONLY);

	return mem_bw_support ? 0 : -1;
}

/* ============================================================================
 * READING
 * ============================================================================ */

/*
 * Read raw memory bandwidth counter (in bytes)
 */
int read_mem_bw_raw_checked(unsigned long long *counter)
{
	unsigned long long total = 0;

	if (!counter || !mem_bw_support)
		return -1;

	if (mem_bw_read_fd >= 0) {
		if (fd_read_ull_checked(mem_bw_read_fd, &total) == 0) {
			*counter = total;
			return 0;
		}
		close(mem_bw_read_fd);
		mem_bw_read_fd = -1;
	}

	if (mem_bw_read_path[0] &&
	    sysfs_read_ull_checked(mem_bw_read_path, &total) == 0) {
		mem_bw_read_fd = open(mem_bw_read_path, O_RDONLY);
		*counter = total;
		return 0;
	}

	return -1;
}

/*
 * Calculate memory bandwidth from delta
 *
 * @current: current counter value
 * @previous: previous counter value
 * @now_us: delta or current timestamp (microseconds)
 * @prev_us: baseline timestamp (microseconds)
 */
static double calculate_mem_bw_delta(unsigned long long current,
				     unsigned long long previous,
				     unsigned long long now_us,
				     unsigned long long prev_us)
{
	unsigned long long delta_bytes;
	unsigned long long delta_us;
	double bw;

	if (current < previous)
		return 0;  /* Counter wrapped or reset */

	delta_bytes = current - previous;
	delta_us = now_us - prev_us;

	if (delta_us == 0)
		return 0;

	/* Bandwidth in MiB/s = (bytes / 1024 / 1024) / (us / 1000000)
	 *                = bytes * 1000000 / 1024 / 1024 / us
	 *                = bytes / 1048.576 / us * 1000000
	 * Simpler: bytes per second = bytes * 1000000 / us
	 *          MiB/s = (bytes * 1000000 / us) / 1024 / 1024
	 * Floating-point scaling avoids overflowing an integer intermediate. */
	bw = ((double)delta_bytes * 1000000.0) /
		(double)delta_us / (1024.0 * 1024.0);

	return bw;
}

/*
 * Update memory bandwidth interval statistics
 *
 * @delta_us: time elapsed since last update (microseconds)
 * @mem_bw_counter: current memory bandwidth counter value (bytes)
 */
void update_mem_bw_interval_stats(unsigned long long delta_us,
				  unsigned long long mem_bw_counter,
				  int counter_valid)
{
	if (!counter_valid) {
		interval_mem_bw = NAN;
		mem_bw_initialized = 0;
		return;
	}

	/* First call, recovery, or explicit baseline reset. */
	if (!mem_bw_initialized || delta_us == 0) {
		prev_mem_read = mem_bw_counter;
		interval_mem_bw = delta_us == 0 ? 0.0 : NAN;
		mem_bw_initialized = 1;
		return;
	}
	if (mem_bw_counter < prev_mem_read) {
		prev_mem_read = mem_bw_counter;
		interval_mem_bw = NAN;
		return;
	}

	/* Use the collector's unified interval for consistency across metrics. */
	interval_mem_bw = calculate_mem_bw_delta(mem_bw_counter, prev_mem_read,
						delta_us, 0);

	prev_mem_read = mem_bw_counter;
}

/*
 * Get memory bandwidth for the interval (MiB/s)
 */
double get_interval_mem_bw(void)
{
	return interval_mem_bw;
}

/*
 * Check if memory bandwidth is supported
 */
int get_mem_bw_support(void)
{
	return mem_bw_support;
}

const char *get_mem_bw_source_path(void)
{
	return mem_bw_support && mem_bw_read_path[0] ? mem_bw_read_path : NULL;
}

int get_mem_bw_candidate_count(void)
{
	return mem_bw_candidate_count;
}

void reset_mem_bw(void)
{
	prev_mem_read = 0;
	interval_mem_bw = NAN;
	mem_bw_initialized = 0;
}

/*
 * Close memory bandwidth monitoring
 */
void close_mem_bw(void)
{
	if (mem_bw_read_fd >= 0) {
		close(mem_bw_read_fd);
		mem_bw_read_fd = -1;
	}
	reset_mem_bw();
	mem_bw_support = 0;
	mem_bw_candidate_count = 0;
	mem_bw_read_path[0] = '\0';
}
