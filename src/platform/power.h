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

/* Checked package-power read. Returns 0 on success, -1 if unavailable. */
int read_total_power_mw(long long *power_mw);

/* Checked bulk read; valid_mask bit N marks temps[N] as current. */
int read_all_numa_temps_checked(int *temps, unsigned int *valid_mask,
				int max_numas);

/* Capability / inventory queries */

/* Indexed NUMA span represented by temperature data (highest node + 1). */
int get_temp_numa_count(void);

/* Discovered sensor count and node-index mask. */
int get_temp_numa_sensor_count(void);
unsigned int get_temp_numa_mask(void);
const char *get_package_power_source_path(void);
int get_package_power_candidate_count(void);

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
void update_power_interval_stats(unsigned long long delta_us,
				 unsigned long long current_power,
				 int current_valid);
double get_interval_avg_power_mw(void);

/* ============================================================================
 * MEMORY BANDWIDTH MODULE (membw.c)
 * Memory bandwidth tracking
 * ============================================================================ */

/* Update memory bandwidth interval stats */
void update_mem_bw_interval_stats(unsigned long long delta_us,
				  unsigned long long mem_bw_counter,
				  int counter_valid);
void reset_mem_bw(void);

/* Get interval memory bandwidth (MiB/s) */
double get_interval_mem_bw(void);

/* Check if memory bandwidth is supported */
int get_mem_bw_support(void);
const char *get_mem_bw_source_path(void);
int get_mem_bw_candidate_count(void);

/* Checked raw counter read. Returns 0 on success, -1 if unavailable. */
int read_mem_bw_raw_checked(unsigned long long *counter);

#endif /* ARMSTAT_POWER_H */
