/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_POWER_H
#define ARMSTAT_POWER_H

#define POWER_SYSFS_PATH_LEN 512

/* ============================================================================
 * POWER FACADE
 * Stable API consumed by collector / formatter layers.
 * ============================================================================ */

/* Initialize and cleanup the power subsystem.
 * Discovers hwmon power_meter and thermal_zone sensors. */
int init_power(void);
void close_power(void);

/* Raw power / temperature readings */

/* Package power in mW — reads hwmon power1_average every call.
 * This is NOT a cached getter; each call performs sysfs I/O.
 * Returns 0 if no power_meter sensor is present on this platform. */
long long get_total_power(void);

/* Bulk read for all tracked CPUs — fills powers[0..max_cpus-1] in tracked-CPU order. */
int read_all_cpu_power(long long *powers, int max_cpus);

/* Bulk read for all tracked CPUs — fills temps[0..max_cpus-1] in tracked-CPU order. */
int read_all_cpu_temp(int *temps, int max_cpus);

/* Read NUMA-level temperatures from thermal_zoneN/temp (milli-C).
 * Fills temps[0..max_numas-1]; actual count available via get_temp_numa_count(). */
int read_all_numa_temps(int *temps, int max_numas);

/* Capability / inventory queries */

/* Number of NUMA nodes with temperature sensors. */
int get_temp_numa_count(void);

/* Whether per-core power telemetry is available on this platform. */
int get_per_core_power_support(void);

/* Human-readable name of the active summary temperature policy
 * (e.g. "thermal-zone-index" or "none"). */
const char *get_summary_temp_policy_name(void);

/* ============================================================================
 * POWER INTERVAL MODULE (power_interval.c)
 * Interval-based power/energy calculations
 * ============================================================================ */

/* Energy in Joules */
double get_interval_energy_joules(void);  /* Joules for last interval */
void reset_energy(void);

/* Interval-based statistics */
void update_power_interval_stats(unsigned long long delta_us, unsigned long long current_power);
long long get_interval_avg_power_mw(void);

/* ============================================================================
 * MEMORY BANDWIDTH MODULE (membw.c)
 * Memory bandwidth tracking
 * ============================================================================ */

/* Update memory bandwidth interval stats */
void update_mem_bw_interval_stats(unsigned long long delta_us, unsigned long long mem_bw_counter);
void reset_mem_bw(void);

/* Get interval memory bandwidth (MB/s) */
unsigned long long get_interval_mem_bw(void);

/* Check if memory bandwidth is supported */
int get_mem_bw_support(void);

/* Raw counter reading */
unsigned long long read_mem_bw_raw(void);

#endif /* ARMSTAT_POWER_H */
