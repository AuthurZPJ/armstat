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

#include "power.h"

/* Memory bandwidth state */
static int mem_bw_support;
static FILE *mem_bw_read_fp;
static char mem_bw_read_path[POWER_SYSFS_PATH_LEN];
static unsigned long long prev_mem_read;
static unsigned long long interval_mem_bw;

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
	int found = 0;

	mem_bw_read_path[0] = '\0';

	/* Look in /sys/class/memory/ */
	dir = opendir("/sys/class/memory");
	if (dir) {
		while ((entry = readdir(dir)) != NULL) {
			/* Look for mem* directories */
			if (strncmp(entry->d_name, "mem", 3) == 0) {
				char path[POWER_SYSFS_PATH_LEN];
				FILE *fp;

				/* Check for mem_bytes_read */
				snprintf(path, sizeof(path),
					 "/sys/class/memory/%s/mem_bytes_read",
					 entry->d_name);
				fp = fopen(path, "r");
				if (fp) {
					fclose(fp);
					snprintf(mem_bw_read_path,
						 sizeof(mem_bw_read_path),
						 "%s", path);
					found = 1;
					break;
				}
			}
		}
		closedir(dir);
	}

	return found;
}

/*
 * Initialize memory bandwidth monitoring (public)
 */
int init_mem_bw(void)
{
	/* Idempotency: close existing fp before re-initializing */
	if (mem_bw_read_fp) {
		fclose(mem_bw_read_fp);
		mem_bw_read_fp = NULL;
	}

	/* Scan for memory bandwidth counters */
	mem_bw_support = scan_mem_bw_counters();

	if (mem_bw_support && mem_bw_read_path[0])
		mem_bw_read_fp = fopen(mem_bw_read_path, "r");

	return mem_bw_support ? 0 : -1;
}

/* ============================================================================
 * READING
 * ============================================================================ */

/*
 * Read raw memory bandwidth counter (in bytes)
 */
unsigned long long read_mem_bw_raw(void)
{
	unsigned long long total = 0;

	if (!mem_bw_support)
		return 0;

	if (mem_bw_read_fp) {
		rewind(mem_bw_read_fp);
		if (fscanf(mem_bw_read_fp, "%llu", &total) != 1)
			total = 0;
		return total;
	}

	if (mem_bw_read_path[0]) {
		FILE *fp = fopen(mem_bw_read_path, "r");

		if (fp) {
			if (fscanf(fp, "%llu", &total) != 1)
				total = 0;
			fclose(fp);
		}
	}

	return total;
}

/*
 * Calculate memory bandwidth from delta
 *
 * @current: current counter value
 * @previous: previous counter value
 * @now_us: delta or current timestamp (microseconds)
 * @prev_us: baseline timestamp (microseconds)
 */
static unsigned long long calculate_mem_bw_delta(unsigned long long current,
						 unsigned long long previous,
						 unsigned long long now_us,
						 unsigned long long prev_us)
{
	unsigned long long delta_bytes;
	unsigned long long delta_us;
	unsigned long long bw;

	if (current < previous)
		return 0;  /* Counter wrapped or reset */

	delta_bytes = current - previous;
	delta_us = now_us - prev_us;

	if (delta_us == 0)
		return 0;

	/* Bandwidth in MB/s = (bytes / 1024 / 1024) / (us / 1000000)
	 *                = bytes * 1000000 / 1024 / 1024 / us
	 *                = bytes / 1048.576 / us * 1000000
	 * Simpler: bytes per second = bytes * 1000000 / us
	 *          MB/s = (bytes * 1000000 / us) / 1024 / 1024
	 * Divide first to avoid overflow on long intervals + high bandwidth. */
	bw = (delta_bytes / delta_us) * 1000000ULL;  /* bytes/sec */
	bw += ((delta_bytes % delta_us) * 1000000ULL) / delta_us;  /* remainder */
	bw = bw / (1024 * 1024);  /* MB/s */

	return bw;
}

/*
 * Update memory bandwidth interval statistics
 *
 * @delta_us: time elapsed since last update (microseconds)
 * @mem_bw_counter: current memory bandwidth counter value (bytes)
 */
void update_mem_bw_interval_stats(unsigned long long delta_us, unsigned long long mem_bw_counter)
{
	/* First call or explicit baseline reset */
	if (delta_us == 0) {
		prev_mem_read = mem_bw_counter;
		interval_mem_bw = 0;
		return;
	}

	/* Use the collector's unified interval for consistency across metrics. */
	interval_mem_bw = calculate_mem_bw_delta(mem_bw_counter, prev_mem_read,
						delta_us, 0);

	prev_mem_read = mem_bw_counter;
}

/*
 * Get memory bandwidth for the interval (MB/s)
 */
unsigned long long get_interval_mem_bw(void)
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

void reset_mem_bw(void)
{
	prev_mem_read = 0;
	interval_mem_bw = 0;
}

/*
 * Close memory bandwidth monitoring
 */
void close_mem_bw(void)
{
	if (mem_bw_read_fp) {
		fclose(mem_bw_read_fp);
		mem_bw_read_fp = NULL;
	}
	reset_mem_bw();
	mem_bw_support = 0;
	mem_bw_read_path[0] = '\0';
}
