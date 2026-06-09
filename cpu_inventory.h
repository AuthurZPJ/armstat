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
 * cpu_catalog - Centralized CPU information store.
 * Used directly by topology and indirectly by the collector wrappers below.
 */
struct cpu_catalog {
	struct cpu_desc cpus[MAX_CPUS];

	int present_count;       /* Total present CPUs */
	int online_count;        /* Total online CPUs */
	int tracked_count;       /* Online CPUs limited by MAX_CPUS */
};

/*
 * CPU Inventory - maintains actual online CPU list
 * Handles CPU ID mapping: tracked_idx <-> real cpu_id
 */
struct cpu_inventory {
	int present_count;        /* All CPUs that exist */
	int online_count;        /* Currently online CPUs */
	int tracked_count;       /* Effective CPUs being tracked (limited by MAX_CPUS) */

	int present_cpus[MAX_PRESENT_CPUS];   /* List of present CPU IDs */
	int online_cpus[MAX_PRESENT_CPUS];    /* List of online CPU IDs */
	int tracked_cpus[MAX_PRESENT_CPUS];   /* List of tracked (effective) CPU IDs */

	unsigned int generation;  /* Incremented when CPU state changes */
};

/* Global instance - exposed for collector */
extern struct cpu_inventory cpu_inv;
extern struct cpu_catalog cpu_catalog;

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

/* Unified catalog API */
int cpu_catalog_init(void);
int cpu_catalog_rebuild(void);
struct cpu_desc *cpu_catalog_get_by_id(int cpu_id);
struct cpu_desc *cpu_catalog_get_by_present_idx(int idx);
struct cpu_desc *cpu_catalog_get_by_tracked_idx(int idx);
int cpu_catalog_online_count(void);
int cpu_catalog_present_count(void);
int cpu_catalog_tracked_count(void);
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
 * Get CPU-ID indexed array size.
 *
 * Returns the highest present CPU ID plus one, not the number of present
 * CPUs. This keeps sparse CPU IDs and hotplug-created holes safe when a
 * caller indexes arrays by real Linux CPU ID.
 */
int get_cpu_id_array_size(void);

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
