/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cpu_inventory.c - Unified CPU discovery and inventory management
 *
 * Single source of truth for:
 *   - CPU discovery from /sys/devices/system/cpu
 *   - present / online / tracked CPU sets
 *   - cached topology attributes filled by topology.c
 *   - compatibility wrappers for tracked_idx -> cpu_id consumers
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "cpu_inventory.h"

struct cpu_catalog cpu_catalog;
struct cpu_inventory cpu_inv;

static int catalog_initialized;
static int inventory_initialized;
static int pending_tracked_valid;
static int pending_tracked_count;
static int pending_tracked_cpus[MAX_PRESENT_CPUS];
static int cpu_filter_enabled;
static int cpu_filter_count;
static int cpu_filter_cpus[MAX_PRESENT_CPUS];

int cpu_filter_contains(int cpu_id)
{
	for (int i = 0; i < cpu_filter_count; i++) {
		if (cpu_filter_cpus[i] == cpu_id)
			return 1;
	}
	return 0;
}

static int cpu_filter_add(int cpu_id)
{
	if (cpu_id < 0 || cpu_id >= MAX_CPUS)
		return -1;
	if (cpu_filter_contains(cpu_id))
		return 0;
	if (cpu_filter_count >= MAX_PRESENT_CPUS)
		return -1;

	cpu_filter_cpus[cpu_filter_count++] = cpu_id;
	return 0;
}

static char *trim_token(char *token)
{
	char *end;

	while (*token && isspace((unsigned char)*token))
		token++;

	end = token + strlen(token);
	while (end > token && isspace((unsigned char)end[-1]))
		*--end = '\0';

	return token;
}

static int parse_cpu_id_strict(const char *text, int *cpu_id)
{
	char *end = NULL;
	long value;

	if (!text || !*text)
		return -1;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || end == text || value < 0 || value >= MAX_CPUS)
		return -1;

	while (end && *end) {
		if (!isspace((unsigned char)*end))
			return -1;
		end++;
	}

	*cpu_id = (int)value;
	return 0;
}

static int cpu_filter_has_empty_token(const char *list)
{
	int need_value = 1;

	for (const char *p = list; p && *p; p++) {
		if (*p == ',') {
			if (need_value)
				return 1;
			need_value = 1;
			continue;
		}
		if (!isspace((unsigned char)*p))
			need_value = 0;
	}

	return need_value;
}

static int parse_cpu_filter_list(const char *list)
{
	char *copy;
	char *token;
	char *saveptr = NULL;

	if (!list || !*list || cpu_filter_has_empty_token(list)) {
		fprintf(stderr, "Error: --cpu requires a non-empty CPU list\n");
		return -1;
	}

	copy = strdup(list);
	if (!copy)
		return -1;

	cpu_filter_count = 0;
	for (token = strtok_r(copy, ",", &saveptr);
	     token;
	     token = strtok_r(NULL, ",", &saveptr)) {
		char *dash;
		int start;
		int end;

		token = trim_token(token);
		if (!*token) {
			fprintf(stderr, "Error: --cpu contains an empty token\n");
			free(copy);
			return -1;
		}

		dash = strchr(token, '-');
		if (dash) {
			*dash = '\0';
			if (strchr(dash + 1, '-')) {
				fprintf(stderr, "Error: invalid --cpu range '%s-%s'\n",
					token, dash + 1);
				free(copy);
				return -1;
			}
			if (parse_cpu_id_strict(trim_token(token), &start) < 0 ||
			    parse_cpu_id_strict(trim_token(dash + 1), &end) < 0 ||
			    start > end) {
				fprintf(stderr, "Error: invalid --cpu range\n");
				free(copy);
				return -1;
			}
			for (int cpu = start; cpu <= end; cpu++) {
				if (cpu_filter_add(cpu) < 0) {
					free(copy);
					return -1;
				}
			}
		} else {
			if (parse_cpu_id_strict(token, &start) < 0 ||
			    cpu_filter_add(start) < 0) {
				fprintf(stderr, "Error: invalid --cpu id '%s'\n", token);
				free(copy);
				return -1;
			}
		}
	}

	free(copy);
	if (cpu_filter_count <= 0) {
		fprintf(stderr, "Error: --cpu did not contain any CPU IDs\n");
		return -1;
	}

	return 0;
}

static int catalog_cpu_is_tracked(const struct cpu_desc *cpu)
{
	if (!cpu || !cpu->online)
		return 0;
	if (!cpu_filter_enabled)
		return 1;
	return cpu_filter_contains(cpu->cpu_id);
}

static int compare_cpu_desc_by_id(const void *lhs, const void *rhs)
{
	const struct cpu_desc *left = lhs;
	const struct cpu_desc *right = rhs;

	if (left->cpu_id < right->cpu_id)
		return -1;
	if (left->cpu_id > right->cpu_id)
		return 1;
	return 0;
}

static void sort_present_cpus(void)
{
	if (cpu_catalog.present_count > 1) {
		qsort(cpu_catalog.cpus, cpu_catalog.present_count,
		      sizeof(cpu_catalog.cpus[0]), compare_cpu_desc_by_id);
	}
}

static void restore_topology_attributes(const struct cpu_catalog *previous)
{
	for (int i = 0; i < cpu_catalog.present_count; i++) {
		struct cpu_desc *current = &cpu_catalog.cpus[i];

		for (int j = 0; j < previous->present_count; j++) {
			const struct cpu_desc *old = &previous->cpus[j];

			if (old->cpu_id != current->cpu_id)
				continue;

			current->package_id = old->package_id;
			current->core_id = old->core_id;
			current->numa_node = old->numa_node;
			current->cpu_id_in_core = old->cpu_id_in_core;
			current->cores_per_socket = old->cores_per_socket;
			current->cpus_per_core = old->cpus_per_core;
			break;
		}
	}
}

static int catalog_membership_changed(const struct cpu_catalog *previous)
{
	if (cpu_catalog.present_count != previous->present_count)
		return 1;
	if (cpu_catalog_online_count() != previous->online_count)
		return 1;
	if (cpu_catalog.tracked_count != previous->tracked_count)
		return 1;

	/*
	 * Use previous->present_count as the loop bound since both arrays
	 * are valid at this point. If the current catalog grew, the count
	 * mismatch above already caught it.
	 */
	for (int i = 0; i < previous->present_count; i++) {
		const struct cpu_desc *current = &cpu_catalog.cpus[i];
		const struct cpu_desc *old = &previous->cpus[i];

		if (current->cpu_id != old->cpu_id || current->online != old->online)
			return 1;
	}

	return 0;
}

static void do_catalog_scan(void)
{
	DIR *dir;
	struct dirent *entry;

	memset(&cpu_catalog, 0, sizeof(cpu_catalog));

	dir = opendir("/sys/devices/system/cpu");
	if (!dir)
		return;

	while ((entry = readdir(dir)) != NULL) {
		int cpu_id;

		if (strncmp(entry->d_name, "cpu", 3) != 0)
			continue;
		if (entry->d_name[3] < '0' || entry->d_name[3] > '9')
			continue;

		cpu_id = atoi(entry->d_name + 3);
		if (cpu_id < 0 || cpu_id >= MAX_CPUS)
			continue;

		cpu_catalog.cpus[cpu_catalog.present_count].cpu_id = cpu_id;
		cpu_catalog.cpus[cpu_catalog.present_count].present = 1;
		cpu_catalog.present_count++;
	}
	closedir(dir);

	sort_present_cpus();

	/*
	 * Most systems expose /sys/devices/system/cpu/online, but on platforms
	 * where it is missing we still want armstat to track all present CPUs
	 * rather than silently ending up with an empty online set.
	 */
	for (int i = 0; i < cpu_catalog.present_count; i++)
		cpu_catalog.cpus[i].online = 1;

	{
		FILE *fp = fopen("/sys/devices/system/cpu/online", "r");

		if (fp) {
			char line[256];

			for (int i = 0; i < cpu_catalog.present_count; i++)
				cpu_catalog.cpus[i].online = 0;

			while (fgets(line, sizeof(line), fp)) {
				char *token;
				char *saveptr = NULL;
				char *nl = strchr(line, '\n');

				if (nl)
					*nl = '\0';

				token = strtok_r(line, ",", &saveptr);
				while (token) {
					int start, end;

					if (sscanf(token, "%d-%d", &start, &end) == 2) {
						for (int cpu = start; cpu <= end && cpu < MAX_CPUS; cpu++) {
							for (int i = 0; i < cpu_catalog.present_count; i++) {
								if (cpu_catalog.cpus[i].cpu_id == cpu) {
									cpu_catalog.cpus[i].online = 1;
									break;
								}
							}
						}
					} else if (sscanf(token, "%d", &start) == 1) {
						for (int i = 0; i < cpu_catalog.present_count; i++) {
							if (cpu_catalog.cpus[i].cpu_id == start) {
								cpu_catalog.cpus[i].online = 1;
								break;
							}
						}
					}

					token = strtok_r(NULL, ",", &saveptr);
				}
			}
			fclose(fp);
		}
	}

	/*
	 * Membership comparisons rely on the cached aggregate counts living in
	 * cpu_catalog. Keep online_count in sync with the freshly scanned set so
	 * rebuild detection only fires on real topology/online-mask changes.
	 */
	cpu_catalog.online_count = 0;
	cpu_catalog.tracked_count = 0;
	for (int i = 0; i < cpu_catalog.present_count; i++) {
		if (cpu_catalog.cpus[i].online)
			cpu_catalog.online_count++;
		if (cpu_catalog.tracked_count < MAX_CPUS &&
		    catalog_cpu_is_tracked(&cpu_catalog.cpus[i]))
			cpu_catalog.tracked_count++;
	}
}

static void inventory_build(void)
{
	cpu_inv.present_count = cpu_catalog.present_count;
	cpu_inv.online_count = cpu_catalog_online_count();
	cpu_inv.tracked_count = cpu_catalog_tracked_count();

	for (int i = 0; i < cpu_inv.tracked_count && i < MAX_PRESENT_CPUS; i++) {
		struct cpu_desc *desc = cpu_catalog_get_by_tracked_idx(i);

		cpu_inv.tracked_cpus[i] = desc ? desc->cpu_id : -1;
	}

	for (int i = 0; i < cpu_inv.present_count && i < MAX_PRESENT_CPUS; i++) {
		cpu_inv.present_cpus[i] = cpu_catalog.cpus[i].cpu_id;
		cpu_inv.online_cpus[i] = cpu_catalog.cpus[i].online ?
					 cpu_catalog.cpus[i].cpu_id : -1;
	}

	cpu_inv.generation++;
}

int cpu_catalog_init(void)
{
	if (catalog_initialized && cpu_catalog.present_count > 0)
		return 0;

	do_catalog_scan();
	catalog_initialized = 1;
	return 0;
}

int cpu_catalog_rebuild(void)
{
	struct cpu_catalog previous = cpu_catalog;
	int changed;

	previous.online_count = cpu_catalog_online_count();
	previous.tracked_count = cpu_catalog_tracked_count();
	do_catalog_scan();
	restore_topology_attributes(&previous);

	changed = catalog_membership_changed(&previous);
	return changed;
}

struct cpu_desc *cpu_catalog_get_by_id(int cpu_id)
{
	for (int i = 0; i < cpu_catalog.present_count; i++) {
		if (cpu_catalog.cpus[i].cpu_id == cpu_id)
			return &cpu_catalog.cpus[i];
	}
	return NULL;
}

struct cpu_desc *cpu_catalog_get_by_present_idx(int idx)
{
	if (idx < 0 || idx >= cpu_catalog.present_count)
		return NULL;
	return &cpu_catalog.cpus[idx];
}

struct cpu_desc *cpu_catalog_get_by_tracked_idx(int idx)
{
	int count = 0;

	if (idx < 0 || idx >= cpu_catalog.tracked_count)
		return NULL;

	for (int i = 0; i < cpu_catalog.present_count; i++) {
		if (!catalog_cpu_is_tracked(&cpu_catalog.cpus[i]))
			continue;
		if (count == idx)
			return &cpu_catalog.cpus[i];
		count++;
	}
	return NULL;
}

int cpu_catalog_online_count(void)
{
	int count = 0;

	for (int i = 0; i < cpu_catalog.present_count; i++) {
		if (cpu_catalog.cpus[i].online)
			count++;
	}
	cpu_catalog.online_count = count;
	return count;
}

int cpu_catalog_present_count(void)
{
	return cpu_catalog.present_count;
}

int cpu_catalog_tracked_count(void)
{
	return cpu_catalog.tracked_count;
}

int set_cpu_inventory_filter(const char *cpu_list)
{
	if (!cpu_list || !*cpu_list) {
		cpu_filter_enabled = 0;
		cpu_filter_count = 0;
		return 0;
	}

	if (parse_cpu_filter_list(cpu_list) < 0) {
		cpu_filter_enabled = 0;
		cpu_filter_count = 0;
		return -1;
	}

	cpu_filter_enabled = 1;
	return 0;
}

int cpu_inventory_filter_is_active(void)
{
	return cpu_filter_enabled;
}

void cpu_catalog_cleanup(void)
{
	memset(&cpu_catalog, 0, sizeof(cpu_catalog));
	catalog_initialized = 0;
}

int init_cpu_inventory(void)
{
	if (inventory_initialized)
		return 0;

	if (cpu_catalog_init() != 0)
		return -1;

	inventory_build();
	if (cpu_filter_enabled && cpu_inv.tracked_count <= 0) {
		fprintf(stderr, "Error: --cpu filter does not match any online CPU\n");
		return -1;
	}
	inventory_initialized = 1;
	return 0;
}

int get_cpu_id_by_tracked_idx(int tracked_idx)
{
	struct cpu_desc *desc = cpu_catalog_get_by_tracked_idx(tracked_idx);

	if (!desc)
		return -1;
	return desc->cpu_id;
}

int get_tracked_cpu_count(void)
{
	return cpu_catalog_tracked_count();
}

int check_and_rebuild_inventory(void)
{
	struct cpu_catalog prev_catalog = cpu_catalog;
	struct cpu_inventory prev_inventory = cpu_inv;
	int prev_tracked_count = cpu_inv.tracked_count;
	static int prev_tracked_cpus[MAX_PRESENT_CPUS];
	int tracked_changed = 0;

	if (prev_tracked_count > 0) {
		memcpy(prev_tracked_cpus, cpu_inv.tracked_cpus,
		       prev_tracked_count * sizeof(prev_tracked_cpus[0]));
	}

	/*
	 * Re-scan every time we are asked, but only report a rebuild when the
	 * tracked CPU set armstat actually samples has changed. This keeps
	 * runtime-state rebuilds tied to meaningful execution changes instead of
	 * incidental catalog bookkeeping differences.
	 */
	cpu_catalog_rebuild();
	inventory_build();

	/*
	 * A running armstat instance cannot legitimately lose every tracked CPU in
	 * one scan. Treat an empty tracked set as a transient inventory/read
	 * failure and keep the previous catalog so we do not tear down the runtime
	 * state on obviously bogus input.
	 */
	if (!cpu_filter_enabled && prev_tracked_count > 0 &&
	    cpu_inv.tracked_count <= 0) {
		cpu_catalog = prev_catalog;
		cpu_inv = prev_inventory;
		pending_tracked_valid = 0;
		return 0;
	}

	if (cpu_inv.tracked_count != prev_tracked_count) {
		tracked_changed = 1;
	} else if (prev_tracked_count > 0 &&
		   memcmp(prev_tracked_cpus, cpu_inv.tracked_cpus,
			  prev_tracked_count * sizeof(prev_tracked_cpus[0])) != 0) {
		tracked_changed = 1;
	}

	if (!tracked_changed) {
		pending_tracked_valid = 0;
		pending_tracked_count = 0;
		return 0;
	}

	/*
	 * Require the same new tracked-CPU set to be observed twice before we
	 * rebuild runtime state. This filters out transient sysfs/inventory
	 * glitches that would otherwise look like a hotplug storm on the first
	 * sampled interval.
	 */
	if (pending_tracked_valid &&
	    pending_tracked_count == cpu_inv.tracked_count &&
	    (pending_tracked_count == 0 ||
	     memcmp(pending_tracked_cpus, cpu_inv.tracked_cpus,
		    pending_tracked_count * sizeof(pending_tracked_cpus[0])) == 0)) {
		pending_tracked_valid = 0;
		pending_tracked_count = 0;
		return 1;
	}

	pending_tracked_count = cpu_inv.tracked_count;
	pending_tracked_valid = 1;
	if (pending_tracked_count > 0) {
		memcpy(pending_tracked_cpus, cpu_inv.tracked_cpus,
		       pending_tracked_count * sizeof(pending_tracked_cpus[0]));
	}

	cpu_catalog = prev_catalog;
	cpu_inv = prev_inventory;
	return 0;
}

int get_cpu_id_array_size(void)
{
	if (!catalog_initialized || cpu_catalog.present_count <= 0)
		return -1;

	/* Catalog is sorted by cpu_id; the last entry has the highest ID. */
	return cpu_catalog.cpus[cpu_catalog.present_count - 1].cpu_id + 1;
}

void cleanup_cpu_inventory(void)
{
	memset(&cpu_inv, 0, sizeof(cpu_inv));
	cpu_catalog_cleanup();
	pending_tracked_valid = 0;
	pending_tracked_count = 0;
	cpu_filter_enabled = 0;
	cpu_filter_count = 0;
	inventory_initialized = 0;
}
