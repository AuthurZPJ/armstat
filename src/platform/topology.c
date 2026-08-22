/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ARM CPU topology detection
 *
 * Detects CPU topology including:
 *   - Number of CPUs
 *   - Core per socket mapping
 *   - NUMA node assignment
 *
 * Data sources:
 *   - /sys/devices/system/cpu/cpuN/topology/
 *   - /sys/devices/system/cpu/cpuN/nodeN (NUMA)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "topology.h"
#include "cpu_inventory.h"
#include "sysfs_util.h"

/*
 * Topology summary statistics cached for quick access.
 * The per-CPU source of truth lives in cpu_inventory's catalog.
 */
static int cores_per_socket = 0;
static int sockets = 0;
static int cpus_per_core = 1;
static int numa_nodes = 0;

/*
 * read_cpu_numa_node - Discover NUMA node for a CPU via cpuN/node*
 * @cpu_id: real Linux CPU ID
 *
 * Linux exposes NUMA affinity for a CPU via symlinks such as:
 *   /sys/devices/system/cpu/cpu100/node1
 *
 * These entries are not regular files, so trying to fopen()/fscanf() them
 * fails. Instead, scan the cpu directory and parse the node suffix directly.
 *
 * Returns: NUMA node ID, or -1 if not found
 */
static int read_cpu_numa_node(int cpu_id)
{
	DIR *dir;
	struct dirent *entry;
	char path[256];
	int found_node = -1;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", cpu_id);
	dir = opendir(path);
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		char *end;
		long node_id;
		char *suffix;

		if (strncmp(entry->d_name, "node", 4) != 0)
			continue;

		suffix = entry->d_name + 4;
		if (*suffix == '\0' || !isdigit((unsigned char)*suffix))
			continue;

		errno = 0;
		node_id = strtol(suffix, &end, 10);
		if (errno == ERANGE || end == suffix || *end != '\0' ||
		    node_id < 0 || node_id > INT_MAX)
			continue;

		found_node = (int)node_id;
		break;
	}

	closedir(dir);
	return found_node;
}

static struct cpu_desc *get_present_cpu(int present_idx)
{
	return cpu_catalog_get_by_present_idx(present_idx);
}

static int id_in_list(const int *ids, int count, int id)
{
	for (int i = 0; i < count; i++) {
		if (ids[i] == id)
			return 1;
	}
	return 0;
}

static void populate_cpu_topology_attrs(struct cpu_desc *cpu)
{
	char path[256];

	if (!cpu || !cpu->present)
		return;

	/* Read core_id */
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/core_id",
		 cpu->cpu_id);
	if (sysfs_read_int_checked(path, &cpu->core_id) < 0)
		cpu->core_id = cpu->cpu_id;  /* Fallback to CPU ID */

	/* Read physical_package_id (socket/package) */
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
		 cpu->cpu_id);
	if (sysfs_read_int_checked(path, &cpu->package_id) < 0)
		cpu->package_id = 0;  /* Fallback to package 0 */

	/* Read NUMA node - prefer cpuN/node* symlink membership */
	cpu->numa_node = read_cpu_numa_node(cpu->cpu_id);

	/* If not found, try /sys/devices/system/cpu/cpuN/topology/node_id */
	if (cpu->numa_node < 0) {
		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/topology/node_id",
			 cpu->cpu_id);
		sysfs_read_int_checked(path, &cpu->numa_node);
	}

	if (cpu->numa_node < 0)
		cpu->numa_node = 0;  /* Default to node 0 */
}

static int compute_socket_count(int present)
{
	static int package_ids[MAX_CPUS];
	int count = 0;

	for (int i = 0; i < present; i++) {
		struct cpu_desc *cpu = get_present_cpu(i);

		if (!cpu || !cpu->present || cpu->package_id < 0)
			continue;
		if (!id_in_list(package_ids, count, cpu->package_id) &&
		    count < MAX_CPUS)
			package_ids[count++] = cpu->package_id;
	}

	return count;
}

static int compute_cores_per_socket(int present)
{
	static int package_ids[MAX_CPUS];
	int package_count = 0;
	int max_core_count = 0;

	for (int i = 0; i < present; i++) {
		struct cpu_desc *cpu = get_present_cpu(i);

		if (!cpu || !cpu->present || cpu->package_id < 0)
			continue;
		if (!id_in_list(package_ids, package_count, cpu->package_id) &&
		    package_count < MAX_CPUS)
			package_ids[package_count++] = cpu->package_id;
	}

	for (int pkg_idx = 0; pkg_idx < package_count; pkg_idx++) {
		static int core_ids[MAX_CPUS];
		int core_count = 0;

		for (int i = 0; i < present; i++) {
			struct cpu_desc *cpu = get_present_cpu(i);

			if (!cpu || !cpu->present ||
			    cpu->package_id != package_ids[pkg_idx])
				continue;
			if (!id_in_list(core_ids, core_count, cpu->core_id) &&
			    core_count < MAX_CPUS)
				core_ids[core_count++] = cpu->core_id;
		}

		if (core_count > max_core_count)
			max_core_count = core_count;
	}

	return max_core_count;
}

static int compute_cpus_per_core(int present)
{
	int max_threads = 1;

	for (int i = 0; i < present; i++) {
		struct cpu_desc *cpu = get_present_cpu(i);
		int threads = 0;

		if (!cpu || !cpu->present)
			continue;
		for (int j = 0; j < present; j++) {
			struct cpu_desc *peer = get_present_cpu(j);

			if (peer && peer->present &&
			    peer->package_id == cpu->package_id &&
			    peer->core_id == cpu->core_id)
				threads++;
		}
		if (threads > max_threads)
			max_threads = threads;
	}

	return max_threads;
}

static void update_cpu_summary_attrs(int present, int cores, int threads)
{
	for (int i = 0; i < present && i < MAX_CPUS; i++) {
		struct cpu_desc *cpu = get_present_cpu(i);

		if (!cpu)
			continue;
		cpu->cores_per_socket = cores;
		cpu->cpus_per_core = threads;
	}
}

static int compute_numa_node_count(int present)
{
	static int node_ids[MAX_CPUS];
	int count = 0;

	for (int i = 0; i < present; i++) {
		struct cpu_desc *cpu = get_present_cpu(i);

		if (!cpu || !cpu->present || cpu->numa_node < 0)
			continue;
		if (!id_in_list(node_ids, count, cpu->numa_node) &&
		    count < MAX_CPUS)
			node_ids[count++] = cpu->numa_node;
	}

	return count;
}

static void compute_cpu_id_in_core_positions(int present)
{
	for (int i = 0; i < present; i++) {
		struct cpu_desc *cpu = get_present_cpu(i);
		int pos = 0;

		if (!cpu)
			continue;

		for (int j = 0; j < i; j++) {
			struct cpu_desc *prev = get_present_cpu(j);

			if (prev && prev->package_id == cpu->package_id &&
			    prev->core_id == cpu->core_id)
				pos++;
		}

		cpu->cpu_id_in_core = pos;
	}
}

int init_topology(void)
{
	int present;

	/*
	 * Initialize the unified CPU inventory first - it owns the present /
	 * online / tracked CPU sets that topology metadata attaches to.
	 */
	if (cpu_catalog_init() != 0)
		return -1;

	/* Fill topology attributes for each CPU in the catalog */
	present = cpu_catalog_present_count();
	for (int i = 0; i < present; i++)
		populate_cpu_topology_attrs(get_present_cpu(i));

	/* Calculate topology summary statistics from the catalog view. */
	sockets = compute_socket_count(present);
	cores_per_socket = compute_cores_per_socket(present);
	cpus_per_core = compute_cpus_per_core(present);

	/* Update cpu_desc with calculated values */
	update_cpu_summary_attrs(present, cores_per_socket, cpus_per_core);

	/* Count NUMA nodes */
	numa_nodes = compute_numa_node_count(present);

	/* Calculate cpu_id_in_core (position within core for SMT) */
	compute_cpu_id_in_core_positions(present);

	return 0;
}

int get_core_id(int cpu)
{
	struct cpu_desc *desc = cpu_catalog_get_by_id(cpu);
	if (!desc)
		return -1;
	return desc->core_id;
}

int get_socket_id(int cpu)
{
	struct cpu_desc *desc = cpu_catalog_get_by_id(cpu);
	if (!desc)
		return -1;
	return desc->package_id;
}

int get_package_id(int cpu)
{
	return get_socket_id(cpu);
}

int get_cores_per_socket(void)
{
	return cores_per_socket;
}

int get_socket_count(void)
{
	return sockets;
}

int get_cpus_per_core(void)
{
	return cpus_per_core;
}

/* NUMA support */
int get_numa_node(int cpu)
{
	struct cpu_desc *desc = cpu_catalog_get_by_id(cpu);
	if (!desc)
		return -1;
	return desc->numa_node;
}

int get_numa_node_count(void)
{
	return numa_nodes;
}

void close_topology(void)
{
	/* Reset cached summary statistics */
	cores_per_socket = 0;
	sockets = 0;
	cpus_per_core = 1;
	numa_nodes = 0;
}
