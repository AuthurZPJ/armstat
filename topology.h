/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_TOPOLOGY_H
#define ARMSTAT_TOPOLOGY_H

#include <stdint.h>

/* Initialize topology detection */
int init_topology(void);

/* Get core ID for a CPU */
int get_core_id(int cpu);

/* Get socket ID for a CPU */
int get_socket_id(int cpu);

/* Get package ID (same as socket) */
int get_package_id(int cpu);

/* Get number of cores per socket */
int get_cores_per_socket(void);

/* Get number of sockets */
int get_socket_count(void);

/* Get CPU ID within a core (for SMT) */
int get_cpu_id_in_core(int cpu);

/* Get number of CPUs in a core */
int get_cpus_per_core(void);

/* NUMA support */
int get_numa_node(int cpu);
int get_numa_node_count(void);

/* Cleanup function */
void close_topology(void);

#endif /* ARMSTAT_TOPOLOGY_H */
