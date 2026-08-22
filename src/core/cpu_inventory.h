/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_CPU_INVENTORY_H
#define ARMSTAT_CPU_INVENTORY_H

#include "collector.h"

#define MAX_PRESENT_CPUS MAX_CPUS

/*
 * cpu_desc - Unified CPU descriptor
 * Single source of truth for CPU identity and topology attributes.
 */
struct cpu_desc {
	int cpu_id;              /* Real CPU ID (0, 4, 8, 12...) */

	/* Presence status */
	int present;             /* 1 if exists in /sys */
	int online;              /* 1 if currently online */

	/* Topology attributes (filled by topology module) */
	int package_id;
	int core_id;
	int numa_node;
	int cpu_id_in_core;      /* Position within core for SMT */
	int cores_per_socket;
	int cpus_per_core;
};

/*
 * cpu_inventory_seed - Install a fixed CPU set, used by host unit tests that
 * must not touch /sys. Replaces the internal catalog/inventory wholesale; the
 * tracked set is re-derived from the present/online flags and any active
 * --cpu filter, so the catalog and inventory stay consistent.
 *
 * @cpus:  array of CPU descriptors to install
 * @count: number of entries in @cpus (0 < count <= MAX_CPUS)
 * Returns: 0 on success, -1 on invalid input.
 */
struct cpu_inventory_seed {
	int cpu_id;
	int present;
	int online;
	int package_id;
	int core_id;
	int numa_node;
};

int cpu_inventory_seed(const struct cpu_inventory_seed *cpus, int count);

/*
 * Initialize CPU inventory
 * Scans /sys/devices/system/cpu/ for present/online/tracked CPUs
 */
int init_cpu_inventory(void);

/*
 * Configure an optional tracked-CPU filter before init_cpu_inventory().
 * The list uses real Linux CPU IDs and comma/range syntax such as 0,1,4-7.
 * Returns 0 on valid syntax, -1 on invalid input.
 */
int set_cpu_inventory_filter(const char *cpu_list);
int cpu_inventory_filter_is_active(void);
int cpu_filter_contains(int cpu_id);

/* Strictly parse Linux CPU-list syntax (for example 0,2-4,9) into a mask. */
int parse_cpu_list_mask(const char *text, unsigned char *mask, int mask_len,
			int *count_out);

/* Also return the unique CPU count before the representable mask is clipped. */
int parse_cpu_list_mask_with_total(const char *text, unsigned char *mask,
				   int mask_len, int *count_out,
				   int *total_count_out);

/* Unified catalog API */
int cpu_catalog_init(void);
int cpu_catalog_rebuild(void);
struct cpu_desc *cpu_catalog_get_by_id(int cpu_id);
struct cpu_desc *cpu_catalog_get_by_present_idx(int idx);
struct cpu_desc *cpu_catalog_get_by_tracked_idx(int idx);
int cpu_catalog_online_count(void);
int cpu_catalog_present_count(void);
int cpu_catalog_tracked_count(void);

/* Compare a parsed Linux online mask with the current catalog membership. */
int cpu_catalog_matches_online_mask(const unsigned char *mask, int mask_len,
				    int represented_count, int total_count);
void cpu_catalog_cleanup(void);

/*
 * Get CPU ID by tracked index
 * @tracked_idx: 0-based index into internal arrays
 * Returns: real CPU ID (e.g., 0, 4, 8, 12)
 */
int get_cpu_id_by_tracked_idx(int tracked_idx);

/*
 * Get tracked CPU count
 * Returns: number of CPUs being tracked
 */
int get_tracked_cpu_count(void);

/*
 * Iterate over every tracked CPU, in tracked order.
 *
 * @idx:  int variable receiving the dense tracked index (0..tracked_count-1)
 * @desc: struct cpu_desc * variable receiving each tracked CPU's descriptor
 *
 * The catalog must not be rebuilt during iteration. This is the one place
 * consumers should walk the tracked set — it hides the tracked_idx/cpu_id
 * translation and the bounds check.
 */
#define for_each_tracked_cpu(idx, desc)						\
	for ((idx) = 0, (desc) = cpu_catalog_get_by_tracked_idx((idx));	\
	     (desc) != NULL;							\
	     (idx)++, (desc) = cpu_catalog_get_by_tracked_idx((idx)))

/*
 * Build a sysfs path under /sys/devices/system/cpu/cpu<id>/.
 *
 * @cpu_id:  real Linux CPU ID
 * @subpath: path relative to the cpu directory, e.g. "cpufreq/cpuinfo_cur_freq"
 *           or "cpuidle/state0/time" (state index formatted by the caller)
 * @buf / @buflen: output buffer
 * Returns: 0 on success, -1 if the result would be truncated.
 *
 * The real CPU ID appears in sysfs directory names; keeping that knowledge in
 * one place means consumers can pass a cpu_id without owning the mapping.
 */
int cpu_sysfs_path(int cpu_id, const char *subpath, char *buf, size_t buflen);

/*
 * Check if CPU inventory changed and rebuild if needed
 * Returns: 1 if rebuild triggered, 0 otherwise
 */
int check_and_rebuild_inventory(void);

/*
 * Cleanup CPU inventory
 */
void cleanup_cpu_inventory(void);

#endif /* ARMSTAT_CPU_INVENTORY_H */
