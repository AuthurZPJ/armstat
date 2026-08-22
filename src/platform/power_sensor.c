/* SPDX-License-Identifier: GPL-2.0 */
/*
 * power_sensor.c - Platform power and temperature sensor access
 *
 * The initial Kunpeng platform profile expects:
 *   - One package power source via hwmon "power_meter"/power1_average
 *   - Summary temperatures via thermal_zoneN/temp, where N maps to NUMA/Vdie N
 *     under the current thermal-zone-index policy
 *
 * The current platform does not expose reliable per-core power/temperature
 * telemetry, so those capabilities are intentionally reported as unsupported.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include "power.h"
#include "power_internal.h"
#include "sysfs_util.h"
#define POWER_METER_NAME "power_meter"
#define PACKAGE_POWER_FILE "power1_average"
#define MAX_NUMA_TEMP_SENSORS 16

enum summary_temp_policy {
	SUMMARY_TEMP_POLICY_NONE = 0,
	/*
	 * Platform-specific policy: thermal_zoneN/temp is treated as the summary
	 * temperature for NUMA/Vdie N. Keep it explicit so future platforms can
	 * replace the policy without rewriting the collector/formatter pipeline.
	 */
	SUMMARY_TEMP_POLICY_THERMAL_ZONE_INDEX,
};

struct power_info {
	char sensor_path[POWER_SYSFS_PATH_LEN];
	int is_microwatts;
};

struct temp_info {
	char sensor_path[POWER_SYSFS_PATH_LEN];
	int numa_node;
};

static struct power_info package_power_sensor;
static struct temp_info numa_temp_sensors[MAX_NUMA_TEMP_SENSORS];
static int package_power_available;
static int package_power_candidate_count;
static int numa_temp_sensor_count;
static int numa_temp_span;
static unsigned int numa_temp_sensor_mask;
static enum summary_temp_policy summary_temp_policy;
static int package_power_fd = -1;
static int numa_temp_fds[MAX_NUMA_TEMP_SENSORS] = {
	[0 ... MAX_NUMA_TEMP_SENSORS - 1] = -1
};

/* ============================================================================
 * SENSOR DISCOVERY
 * ============================================================================ */

static void copy_sysfs_path(char dest[POWER_SYSFS_PATH_LEN], const char *src)
{
	if (!src) {
		dest[0] = '\0';
		return;
	}

	snprintf(dest, POWER_SYSFS_PATH_LEN, "%s", src);
}

static const char *summary_temp_policy_to_string(enum summary_temp_policy policy)
{
	switch (policy) {
	case SUMMARY_TEMP_POLICY_THERMAL_ZONE_INDEX:
		return "thermal-zone-index";
	case SUMMARY_TEMP_POLICY_NONE:
	default:
		return "none";
	}
}

static enum summary_temp_policy select_summary_temp_policy(void)
{
	const char *override = getenv("ARMSTAT_TEMP_POLICY");

	if (!override || !*override)
		return SUMMARY_TEMP_POLICY_THERMAL_ZONE_INDEX;

	if (strcmp(override, "thermal-zone-index") == 0)
		return SUMMARY_TEMP_POLICY_THERMAL_ZONE_INDEX;
	if (strcmp(override, "none") == 0 || strcmp(override, "disabled") == 0)
		return SUMMARY_TEMP_POLICY_NONE;

	/* Validation rejects unknown strings before discovery reaches this path. */
	return SUMMARY_TEMP_POLICY_NONE;
}

static int validate_summary_temp_policy(void)
{
	const char *override = getenv("ARMSTAT_TEMP_POLICY");

	if (!override || !*override ||
	    strcmp(override, "thermal-zone-index") == 0 ||
	    strcmp(override, "none") == 0 ||
	    strcmp(override, "disabled") == 0)
		return 0;

	fprintf(stderr,
		"Error: unknown ARMSTAT_TEMP_POLICY '%s' "
		"(expected thermal-zone-index, none, or disabled)\n",
		override);
	return -1;
}

static unsigned int discover_numa_node_mask(void)
{
	DIR *dir;
	struct dirent *entry;
	unsigned int mask = 0;

	dir = opendir("/sys/devices/system/node");
	if (!dir)
		return 0;

	while ((entry = readdir(dir)) != NULL) {
		int node_id;
		char tail;

		if (sscanf(entry->d_name, "node%d%c", &node_id, &tail) == 1 &&
		    node_id >= 0 && node_id < MAX_NUMA_TEMP_SENSORS)
			mask |= 1U << node_id;
	}

	closedir(dir);
	return mask;
}

static void reset_sensor_state(void)
{
	if (package_power_fd >= 0) {
		close(package_power_fd);
		package_power_fd = -1;
	}

	memset(&package_power_sensor, 0, sizeof(package_power_sensor));

	for (int i = 0; i < MAX_NUMA_TEMP_SENSORS; i++) {
		if (numa_temp_fds[i] >= 0) {
			close(numa_temp_fds[i]);
			numa_temp_fds[i] = -1;
		}
		memset(&numa_temp_sensors[i], 0, sizeof(numa_temp_sensors[i]));
		numa_temp_sensors[i].numa_node = -1;
	}

	package_power_available = 0;
	package_power_candidate_count = 0;
	numa_temp_sensor_count = 0;
	numa_temp_span = 0;
	numa_temp_sensor_mask = 0;
	summary_temp_policy = SUMMARY_TEMP_POLICY_NONE;
}

static int read_cached_sensor_ull(int *fd, const char *path,
				  unsigned long long *raw)
{
	if (!fd || !path || !raw)
		return -1;

	if (*fd >= 0 && fd_read_ull_checked(*fd, raw) == 0)
		return 0;

	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
	if (sysfs_read_ull_checked(path, raw) < 0)
		return -1;

	/* Best effort: restore the fast-path descriptor after a transient error. */
	*fd = open(path, O_RDONLY);
	return 0;
}

static int read_cached_sensor_int(int *fd, const char *path, int *value)
{
	if (!fd || !path || !value)
		return -1;

	if (*fd >= 0 && fd_read_int_checked(*fd, value) == 0)
		return 0;
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
	if (sysfs_read_int_checked(path, value) < 0)
		return -1;

	*fd = open(path, O_RDONLY);
	return 0;
}

static int read_package_power_value(long long *power_mw)
{
	unsigned long long raw = 0;
	long long value;

	if (!power_mw || !package_power_available)
		return -1;

	if (read_cached_sensor_ull(&package_power_fd,
				   package_power_sensor.sensor_path, &raw) < 0)
		return -1;
	if (raw > LLONG_MAX)
		return -1;

	value = (long long)raw;
	if (package_power_sensor.is_microwatts)
		value /= 1000;

	*power_mw = value;
	return 0;
}

static int discover_package_power_sensor(void)
{
	DIR *hwmon_dir;
	struct dirent *entry;
	char name_path[POWER_SYSFS_PATH_LEN];
	char power_path[POWER_SYSFS_PATH_LEN];
	char matched_path[POWER_SYSFS_PATH_LEN] = "";
	char name_buf[256];
	char *name;

	hwmon_dir = opendir("/sys/class/hwmon");
	if (!hwmon_dir)
		return -1;

	while ((entry = readdir(hwmon_dir)) != NULL) {
		if (strncmp(entry->d_name, "hwmon", 5) != 0)
			continue;

		snprintf(name_path, sizeof(name_path),
			 "/sys/class/hwmon/%s/name", entry->d_name);
		name = sysfs_read_str(name_path, name_buf, sizeof(name_buf));
		if (!*name)
			continue;

		if (strcmp(name, POWER_METER_NAME) != 0)
			continue;

		snprintf(power_path, sizeof(power_path),
			 "/sys/class/hwmon/%s/%s", entry->d_name, PACKAGE_POWER_FILE);
		if (!sysfs_path_exists(power_path))
			continue;

		package_power_candidate_count++;
		if (package_power_candidate_count == 1)
			copy_sysfs_path(matched_path, power_path);
	}

	closedir(hwmon_dir);
	if (package_power_candidate_count != 1)
		return -1;

	copy_sysfs_path(package_power_sensor.sensor_path, matched_path);
	package_power_sensor.is_microwatts = 1;
	package_power_fd = open(matched_path, O_RDONLY);
	package_power_available = 1;
	return 0;
}

static void discover_numa_temp_sensors_direct_index(void)
{
	unsigned int node_mask = discover_numa_node_mask();

	if (!node_mask)
		return;

	for (int zone = 0; zone < MAX_NUMA_TEMP_SENSORS; zone++) {
		char temp_path[POWER_SYSFS_PATH_LEN];
		struct temp_info *sensor;

		if (!(node_mask & (1U << zone)))
			continue;

		snprintf(temp_path, sizeof(temp_path),
			 "/sys/class/thermal/thermal_zone%d/temp", zone);
		if (!sysfs_path_exists(temp_path))
			continue;

		sensor = &numa_temp_sensors[numa_temp_sensor_count];
		copy_sysfs_path(sensor->sensor_path, temp_path);
		sensor->numa_node = zone;
		numa_temp_fds[numa_temp_sensor_count] = open(temp_path, O_RDONLY);
		numa_temp_sensor_count++;
		numa_temp_sensor_mask |= 1U << zone;
		if (zone + 1 > numa_temp_span)
			numa_temp_span = zone + 1;
	}
}

static void discover_numa_temp_sensors(void)
{
	summary_temp_policy = select_summary_temp_policy();

	switch (summary_temp_policy) {
	case SUMMARY_TEMP_POLICY_THERMAL_ZONE_INDEX:
		discover_numa_temp_sensors_direct_index();
		break;
	case SUMMARY_TEMP_POLICY_NONE:
	default:
		break;
	}
}

/*
 * Scan for platform power and temperature sensors
 */
static int scan_power_sensors(void)
{
	reset_sensor_state();

	/*
	 * Package power comes from the single hwmon power_meter device.
	 * Per-core power is not implemented on this platform.
	 */
	discover_package_power_sensor();

	/*
	 * Summary temperature mapping is an explicit policy, not a generic ARM
	 * assumption. The current platform uses direct thermal_zoneN -> NUMA/Vdie N
	 * numbering, and that policy is isolated here so it can be swapped or
	 * disabled without touching the rest of the data path.
	 */
	discover_numa_temp_sensors();

	return 0;
}

/* ============================================================================
 * QUERY FUNCTIONS
 * ============================================================================ */

int get_temp_numa_count(void)
{
	return numa_temp_span;
}

int get_temp_numa_sensor_count(void)
{
	return numa_temp_sensor_count;
}

unsigned int get_temp_numa_mask(void)
{
	return numa_temp_sensor_mask;
}

const char *get_package_power_source_path(void)
{
	return package_power_available ? package_power_sensor.sensor_path : NULL;
}

int get_package_power_candidate_count(void)
{
	return package_power_candidate_count;
}

static int find_temp_sensor_by_numa(int numa_node)
{
	for (int i = 0; i < numa_temp_sensor_count; i++) {
		if (numa_temp_sensors[i].numa_node == numa_node)
			return i;
	}
	return -1;
}

int read_all_numa_temps_checked(int *temps, unsigned int *valid_mask,
				int max_numas)
{
	if (!temps || !valid_mask || max_numas < 0 || max_numas > 16)
		return -1;

	*valid_mask = 0;
	for (int i = 0; i < max_numas; i++)
		temps[i] = 0;

	for (int i = 0; i < max_numas; i++) {
		int sensor_idx = find_temp_sensor_by_numa(i);
		if (sensor_idx < 0)
			continue;

		int value;

		if (read_cached_sensor_int(&numa_temp_fds[sensor_idx],
					   numa_temp_sensors[sensor_idx].sensor_path,
					   &value) < 0)
			continue;
		temps[i] = value;
		*valid_mask |= 1U << i;
	}

	return 0;
}

int get_per_core_power_support(void)
{
	return 0;
}

const char *get_summary_temp_policy_name(void)
{
	return summary_temp_policy_to_string(summary_temp_policy);
}

int read_total_power_mw(long long *power_mw)
{
	return read_package_power_value(power_mw);
}

/*
 * Initialize power sensor subsystem
 */
int init_power_sensor_subsystem(void)
{
	if (validate_summary_temp_policy() < 0)
		return -1;

	/* Scan for sensors */
	scan_power_sensors();

	return 0;
}

/*
 * Close power sensor subsystem
 */
void close_power_sensor_subsystem(void)
{
	reset_sensor_state();
}
