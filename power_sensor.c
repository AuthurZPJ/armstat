/* SPDX-License-Identifier: GPL-2.0 */
/*
 * power_sensor.c - Platform power and temperature sensor access
 *
 * This platform exposes:
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
#include "power.h"
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
	char *sensor_name;
	char sensor_path[POWER_SYSFS_PATH_LEN];
	long long power;
	int is_cpu_power;
	int cpu_id;
	int is_microwatts;
};

struct temp_info {
	char *sensor_name;
	char sensor_path[POWER_SYSFS_PATH_LEN];
	int temp;
	int is_cpu_temp;
	int cpu_id;
	int numa_node;
};

static struct power_info package_power_sensor;
static struct temp_info numa_temp_sensors[MAX_NUMA_TEMP_SENSORS];
static int package_power_available;
static int power_sensor_count;
static int temp_sensor_count;
static int numa_temp_sensor_count;
static enum summary_temp_policy summary_temp_policy;
static int package_power_fd = -1;
static int numa_temp_fds[MAX_NUMA_TEMP_SENSORS] = {
	[0 ... MAX_NUMA_TEMP_SENSORS - 1] = -1
};

/* ============================================================================
 * SYSFS HELPERS
 * ============================================================================ */

static long long read_sysfs_ll(const char *path)
{
	FILE *fp;
	long long value = 0;

	fp = fopen(path, "r");
	if (!fp)
		return 0;

	if (fscanf(fp, "%lld", &value) != 1)
		value = 0;

	fclose(fp);
	return value;
}

static int read_sysfs_int(const char *path)
{
	FILE *fp;
	int value = 0;

	fp = fopen(path, "r");
	if (!fp)
		return 0;

	if (fscanf(fp, "%d", &value) != 1)
		value = 0;

	fclose(fp);
	return value;
}

static char *read_sysfs_str(const char *path, char *buf, size_t len)
{
	FILE *fp;

	if (!buf || len == 0)
		return NULL;

	fp = fopen(path, "r");
	if (!fp)
		return NULL;

	if (fgets(buf, len, fp)) {
		/* Remove trailing newline */
		char *nl = strchr(buf, '\n');
		if (nl) *nl = '\0';
		fclose(fp);
		return buf;
	}

	fclose(fp);
	return NULL;
}

/* ============================================================================
 * SENSOR DISCOVERY
 * ============================================================================ */

/*
 * Read power value, handling microwatts conversion
 */
static int path_exists(const char *path)
{
	return access(path, R_OK) == 0;
}

static void free_sensor_name(char **name)
{
	if (*name) {
		free(*name);
		*name = NULL;
	}
}

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

	/*
	 * Unknown policy strings should not silently invent a new mapping.
	 * Fall back to no summary temperature discovery rather than guessing.
	 */
	return SUMMARY_TEMP_POLICY_NONE;
}

static int count_numa_nodes_sysfs(void)
{
	DIR *dir;
	struct dirent *entry;
	int count = 0;

	dir = opendir("/sys/devices/system/node");
	if (!dir)
		return 0;

	while ((entry = readdir(dir)) != NULL) {
		int node_id;

		if (sscanf(entry->d_name, "node%d", &node_id) == 1)
			count++;
	}

	closedir(dir);
	return count;
}

static long long read_fd_ll(int fd)
{
	char buf[64];
	ssize_t n;

	if (fd < 0)
		return 0;

	lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return 0;

	buf[n] = '\0';
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	return atoll(buf);
}

static int read_fd_int(int fd)
{
	char buf[64];
	ssize_t n;

	if (fd < 0)
		return 0;

	lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0)
		return 0;

	buf[n] = '\0';
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';

	return atoi(buf);
}

static void reset_sensor_state(void)
{
	if (package_power_fd >= 0) {
		close(package_power_fd);
		package_power_fd = -1;
	}

	free_sensor_name(&package_power_sensor.sensor_name);
	memset(&package_power_sensor, 0, sizeof(package_power_sensor));

	for (int i = 0; i < MAX_NUMA_TEMP_SENSORS; i++) {
		if (numa_temp_fds[i] >= 0) {
			close(numa_temp_fds[i]);
			numa_temp_fds[i] = -1;
		}
		free_sensor_name(&numa_temp_sensors[i].sensor_name);
		memset(&numa_temp_sensors[i], 0, sizeof(numa_temp_sensors[i]));
		numa_temp_sensors[i].numa_node = -1;
	}

	package_power_available = 0;
	power_sensor_count = 0;
	temp_sensor_count = 0;
	numa_temp_sensor_count = 0;
	summary_temp_policy = SUMMARY_TEMP_POLICY_NONE;
}

static long long read_package_power_value(void)
{
	long long value;

	if (!package_power_available)
		return 0;

	if (package_power_fd >= 0)
		value = read_fd_ll(package_power_fd);
	else
		value = read_sysfs_ll(package_power_sensor.sensor_path);
	if (package_power_sensor.is_microwatts)
		value /= 1000;

	return value;
}

static int discover_package_power_sensor(void)
{
	DIR *hwmon_dir;
	struct dirent *entry;
	char name_path[POWER_SYSFS_PATH_LEN];
	char power_path[POWER_SYSFS_PATH_LEN];
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
		name = read_sysfs_str(name_path, name_buf, sizeof(name_buf));
		if (!name)
			continue;

		if (strcmp(name, POWER_METER_NAME) != 0)
			continue;

		snprintf(power_path, sizeof(power_path),
			 "/sys/class/hwmon/%s/%s", entry->d_name, PACKAGE_POWER_FILE);
		if (!path_exists(power_path))
			continue;

		package_power_sensor.sensor_name = strdup(POWER_METER_NAME);
		copy_sysfs_path(package_power_sensor.sensor_path, power_path);
		package_power_sensor.is_cpu_power = 1;
		package_power_sensor.cpu_id = -1;
		package_power_sensor.is_microwatts = 1;
		package_power_fd = open(power_path, O_RDONLY);

		package_power_available = 1;
		power_sensor_count = 1;
		closedir(hwmon_dir);
		return 0;
	}

	closedir(hwmon_dir);
	return -1;
}

static void discover_numa_temp_sensors_direct_index(void)
{
	int expected_numa_nodes = count_numa_nodes_sysfs();

	if (expected_numa_nodes <= 0 || expected_numa_nodes > MAX_NUMA_TEMP_SENSORS)
		return;

	for (int zone = 0; zone < expected_numa_nodes; zone++) {
		char temp_path[POWER_SYSFS_PATH_LEN];
		char label[16];
		struct temp_info *sensor;

		snprintf(temp_path, sizeof(temp_path),
			 "/sys/class/thermal/thermal_zone%d/temp", zone);
		if (!path_exists(temp_path))
			continue;

		sensor = &numa_temp_sensors[numa_temp_sensor_count];
		snprintf(label, sizeof(label), "temp%d", zone);
		sensor->sensor_name = strdup(label);
		copy_sysfs_path(sensor->sensor_path, temp_path);
		sensor->is_cpu_temp = 1;
		sensor->cpu_id = -1;
		sensor->numa_node = zone;
		numa_temp_fds[numa_temp_sensor_count] = open(temp_path, O_RDONLY);
		numa_temp_sensor_count++;
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
 * SENSOR READING
 * ============================================================================ */

int read_cpu_power(int cpu, long long *power)
{
	(void)cpu;

	if (!power)
		return -1;

	/*
	 * This platform does not expose per-core/package-to-CPU mapped power.
	 * SUM/package power is available through get_total_power().
	 */
	*power = 0;
	return -1;
}

int read_all_cpu_power(long long *powers, int max_cpus)
{
	if (!powers)
		return -1;

	for (int i = 0; i < max_cpus; i++)
		powers[i] = 0;

	return 0;
}

int read_all_cpu_temp(int *temps, int max_cpus)
{
	if (!temps)
		return -1;

	for (int i = 0; i < max_cpus; i++)
		temps[i] = 0;

	return 0;
}

/* ============================================================================
 * QUERY FUNCTIONS
 * ============================================================================ */

int get_temp_sensor_count(void)
{
	/*
	 * Report per-core temperature capability only.
	 * NUMA/die temperatures are tracked separately via get_temp_numa_count().
	 */
	return temp_sensor_count;
}

int get_temp_numa_count(void)
{
	return numa_temp_sensor_count;
}

static int find_temp_sensor_by_numa(int numa_node)
{
	for (int i = 0; i < numa_temp_sensor_count; i++) {
		if (numa_temp_sensors[i].numa_node == numa_node)
			return i;
	}
	return -1;
}

int read_all_numa_temps(int *temps, int max_numas)
{
	if (!temps)
		return -1;

	for (int i = 0; i < max_numas; i++)
		temps[i] = 0;

	for (int i = 0; i < max_numas; i++) {
		int sensor_idx = find_temp_sensor_by_numa(i);
		if (sensor_idx < 0)
			continue;

		if (numa_temp_fds[sensor_idx] >= 0)
			temps[i] = read_fd_int(numa_temp_fds[sensor_idx]);
		else
			temps[i] = read_sysfs_int(numa_temp_sensors[sensor_idx].sensor_path);
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

long long get_total_power(void)
{
	return read_package_power_value();
}

/*
 * Initialize power sensor subsystem
 */
int init_power_sensor_subsystem(void)
{
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
