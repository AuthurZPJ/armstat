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

/*
 * cpu_catalog - Centralized CPU information store (private; access via the
 * cpu_catalog_* accessors). Holds the per-CPU descriptors plus aggregate
 * present/online/tracked counts.
 */
struct cpu_catalog {
	struct cpu_desc cpus[MAX_CPUS];

	int present_count;       /* Present CPUs representable by this build */
	int online_count;        /* Online CPUs representable by this build */
	int tracked_count;       /* Online CPUs limited by MAX_CPUS */
	int detected_present_count; /* All CPUs reported by sysfs */
	int detected_online_count;  /* All online CPUs reported by sysfs */
};

/*
 * cpu_inventory - Dense CPU ID lists (private; access via get_cpu_id_by_tracked_idx
 * and the for_each_tracked_cpu view). Kept in sync with cpu_catalog by
 * inventory_build().
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

static struct cpu_catalog cpu_catalog;
static struct cpu_inventory cpu_inv;

static int catalog_initialized;
static int inventory_initialized;
static int pending_tracked_valid;
static int pending_tracked_count;
static int pending_tracked_cpus[MAX_PRESENT_CPUS];
static int cpu_filter_enabled;
static int cpu_filter_count;
static int cpu_filter_cpus[MAX_PRESENT_CPUS];
static int online_mask_valid;

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

static int parse_cpu_number(const char *text, int *cpu_id)
{
	char *end = NULL;
	long value;

	if (!text || !*text || !cpu_id)
		return -1;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno || end == text || value < 0 || value > INT_MAX)
		return -1;
	while (*end && isspace((unsigned char)*end))
		end++;
	if (*end)
		return -1;

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

struct cpu_range {
	int start;
	int end;
};

static int compare_cpu_range(const void *lhs, const void *rhs)
{
	const struct cpu_range *left = lhs;
	const struct cpu_range *right = rhs;

	if (left->start != right->start)
		return left->start < right->start ? -1 : 1;
	if (left->end != right->end)
		return left->end < right->end ? -1 : 1;
	return 0;
}

int parse_cpu_list_mask_with_total(const char *text, unsigned char *mask,
				   int mask_len, int *count_out,
				   int *total_count_out)
{
	char *copy;
	char *token;
	char *saveptr = NULL;
	struct cpu_range *ranges = NULL;
	size_t range_count = 0;
	size_t range_capacity = 0;
	int count = 0;
	int merged_start;
	int merged_end;
	long long total_count = 0;

	if (count_out)
		*count_out = 0;
	if (total_count_out)
		*total_count_out = 0;
	if (!text || !*text || !mask || mask_len <= 0 ||
	    cpu_filter_has_empty_token(text))
		return -1;

	memset(mask, 0, (size_t)mask_len);
	copy = strdup(text);
	if (!copy)
		return -1;

	for (token = strtok_r(copy, ",", &saveptr);
	     token;
	     token = strtok_r(NULL, ",", &saveptr)) {
		char *dash;
		int start;
		int end;

		token = trim_token(token);
		dash = strchr(token, '-');
		if (dash) {
			*dash = '\0';
			if (strchr(dash + 1, '-') ||
			    parse_cpu_number(trim_token(token), &start) < 0 ||
			    parse_cpu_number(trim_token(dash + 1), &end) < 0 ||
			    start > end)
				goto fail;
		} else {
			if (parse_cpu_number(token, &start) < 0) {
				goto fail;
			}
			end = start;
		}

		if (range_count == range_capacity) {
			size_t next_capacity = range_capacity ? range_capacity * 2 : 16;
			struct cpu_range *next_ranges;

			if (next_capacity < range_capacity ||
			    next_capacity > SIZE_MAX / sizeof(*ranges))
				goto fail;
			next_ranges = realloc(ranges,
					      next_capacity * sizeof(*ranges));
			if (!next_ranges)
				goto fail;
			ranges = next_ranges;
			range_capacity = next_capacity;
		}
		ranges[range_count].start = start;
		ranges[range_count].end = end;
		range_count++;

		if (start >= mask_len)
			continue;
		if (end >= mask_len)
			end = mask_len - 1;
		for (int cpu = start; cpu <= end; cpu++) {
			if (!mask[cpu]) {
				mask[cpu] = 1;
				count++;
			}
		}
	}

	if (range_count == 0)
		goto fail;
	qsort(ranges, range_count, sizeof(*ranges), compare_cpu_range);
	merged_start = ranges[0].start;
	merged_end = ranges[0].end;

	for (size_t i = 1; i < range_count; i++) {
		if (ranges[i].start <= merged_end) {
			if (ranges[i].end > merged_end)
				merged_end = ranges[i].end;
			continue;
		}
		total_count += (long long)merged_end - merged_start + 1;
		if (total_count > INT_MAX)
			goto fail;
		merged_start = ranges[i].start;
		merged_end = ranges[i].end;
	}
	total_count += (long long)merged_end - merged_start + 1;
	if (total_count > INT_MAX)
		goto fail;

	free(ranges);
	free(copy);
	if (count_out)
		*count_out = count;
	if (total_count_out)
		*total_count_out = (int)total_count;
	return 0;

fail:
	free(ranges);
	free(copy);
	memset(mask, 0, (size_t)mask_len);
	return -1;
}

int parse_cpu_list_mask(const char *text, unsigned char *mask, int mask_len,
			int *count_out)
{
	return parse_cpu_list_mask_with_total(text, mask, mask_len, count_out,
					      NULL);
}

static int parse_cpu_dir_name(const char *name, int *cpu_id)
{
	char *end = NULL;
	long value;

	if (!name || strncmp(name, "cpu", 3) != 0 ||
	    !isdigit((unsigned char)name[3]))
		return -1;

	errno = 0;
	value = strtol(name + 3, &end, 10);
	if (errno || end == name + 3 || *end != '\0' ||
	    value < 0 || value > INT_MAX)
		return -1;

	*cpu_id = (int)value;
	return 0;
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
	if (cpu_catalog.online_count != previous->online_count)
		return 1;
	if (cpu_catalog.detected_present_count !=
	    previous->detected_present_count)
		return 1;
	if (cpu_catalog.detected_online_count !=
	    previous->detected_online_count)
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

static void recompute_catalog_counts(void)
{
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

static int read_online_cpu_mask(unsigned char *mask, int mask_len,
				int *represented_count, int *total_count)
{
	FILE *fp;
	char *line = NULL;
	size_t line_size = 0;
	ssize_t line_len;
	int ret = -1;

	if (!mask || mask_len <= 0 || !represented_count || !total_count)
		return -1;

	*represented_count = 0;
	*total_count = 0;
	fp = fopen("/sys/devices/system/cpu/online", "r");
	if (!fp)
		return -1;

	line_len = getline(&line, &line_size, fp);
	if (line_len > 0 &&
	    parse_cpu_list_mask_with_total(line, mask, mask_len,
					   represented_count, total_count) == 0 &&
	    *total_count > 0)
		ret = 0;

	free(line);
	fclose(fp);
	return ret;
}

int cpu_catalog_matches_online_mask(const unsigned char *mask, int mask_len,
				    int represented_count, int total_count)
{
	if (!mask || mask_len <= 0 || represented_count < 0 || total_count < 0)
		return 0;
	if (represented_count != cpu_catalog.online_count ||
	    total_count != cpu_catalog.detected_online_count)
		return 0;

	for (int i = 0; i < cpu_catalog.present_count; i++) {
		const struct cpu_desc *cpu = &cpu_catalog.cpus[i];

		if (cpu->cpu_id < 0 || cpu->cpu_id >= mask_len)
			return 0;
		if (cpu->online != (mask[cpu->cpu_id] ? 1 : 0))
			return 0;
	}

	return 1;
}

static int online_membership_is_unchanged(void)
{
	unsigned char online_mask[MAX_CPUS];
	int represented_count;
	int total_count;

	if (read_online_cpu_mask(online_mask, MAX_CPUS, &represented_count,
				 &total_count) < 0)
		return 0;

	return cpu_catalog_matches_online_mask(online_mask, MAX_CPUS,
					       represented_count, total_count);
}

static void do_catalog_scan(void)
{
	DIR *dir;
	struct dirent *entry;

	memset(&cpu_catalog, 0, sizeof(cpu_catalog));
	online_mask_valid = 0;

	dir = opendir("/sys/devices/system/cpu");
	if (!dir)
		return;

	while ((entry = readdir(dir)) != NULL) {
		int cpu_id;

		if (parse_cpu_dir_name(entry->d_name, &cpu_id) < 0)
			continue;
		cpu_catalog.detected_present_count++;
		if (cpu_id >= MAX_CPUS || cpu_catalog.present_count >= MAX_CPUS)
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
	cpu_catalog.detected_online_count = cpu_catalog.detected_present_count;

	{
		unsigned char online_mask[MAX_CPUS];
		int mask_count;
		int total_count;

		if (read_online_cpu_mask(online_mask, MAX_CPUS, &mask_count,
					 &total_count) == 0) {
			for (int i = 0; i < cpu_catalog.present_count; i++)
				cpu_catalog.cpus[i].online =
					online_mask[cpu_catalog.cpus[i].cpu_id] ? 1 : 0;
			cpu_catalog.detected_online_count = total_count;
			online_mask_valid = 1;
		}
	}

	/*
	 * Membership comparisons rely on the cached aggregate counts living in
	 * cpu_catalog. Keep online_count in sync with the freshly scanned set so
	 * rebuild detection only fires on real topology/online-mask changes.
	 */
	recompute_catalog_counts();
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
	if (cpu_catalog.present_count <= 0)
		return -1;
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
	if (!online_mask_valid) {
		for (int i = 0; i < cpu_catalog.present_count; i++) {
			struct cpu_desc *current = &cpu_catalog.cpus[i];

			for (int j = 0; j < previous.present_count; j++) {
				if (previous.cpus[j].cpu_id == current->cpu_id) {
					current->online = previous.cpus[j].online;
					break;
				}
			}
		}
		if (cpu_catalog.detected_present_count ==
		    previous.detected_present_count)
			cpu_catalog.detected_online_count =
				previous.detected_online_count;
		recompute_catalog_counts();
	}
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
	return cpu_catalog.detected_online_count;
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
	if (!cpu_list) {
		cpu_filter_enabled = 0;
		cpu_filter_count = 0;
		return 0;
	}
	if (!*cpu_list) {
		fprintf(stderr, "Error: --cpu filter must not be empty\n");
		return -1;
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
	if (tracked_idx < 0 || tracked_idx >= cpu_inv.tracked_count)
		return -1;
	return cpu_inv.tracked_cpus[tracked_idx];
}

int get_tracked_cpu_count(void)
{
	return cpu_catalog_tracked_count();
}

int check_and_rebuild_inventory(void)
{
	static struct cpu_catalog prev_catalog;
	static struct cpu_inventory prev_inventory;
	static int prev_tracked_cpus[MAX_PRESENT_CPUS];
	int prev_tracked_count = cpu_inv.tracked_count;
	int tracked_changed = 0;

	/*
	 * The online mask is the authoritative hotplug signal on Linux and is far
	 * cheaper to read than enumerating every cpuN directory. Keep the full
	 * catalog scan on the change/fallback path, but make the common unchanged
	 * interval a single small-file read.
	 */
	if (online_membership_is_unchanged()) {
		pending_tracked_valid = 0;
		pending_tracked_count = 0;
		return 0;
	}

	prev_catalog = cpu_catalog;
	prev_inventory = cpu_inv;

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
	if (prev_tracked_count > 0 && cpu_catalog.present_count <= 0) {
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

int cpu_sysfs_path(int cpu_id, const char *subpath, char *buf, size_t buflen)
{
	int n;

	if (cpu_id < 0 || !buf || buflen == 0)
		return -1;

	n = snprintf(buf, buflen, "/sys/devices/system/cpu/cpu%d/%s",
		     cpu_id, subpath ? subpath : "");
	if (n < 0 || (size_t)n >= buflen)
		return -1;
	return 0;
}

int cpu_inventory_seed(const struct cpu_inventory_seed *cpus, int count)
{
	if (!cpus || count <= 0 || count > MAX_CPUS)
		return -1;

	for (int i = 0; i < count; i++) {
		if (cpus[i].cpu_id < 0 || cpus[i].cpu_id >= MAX_CPUS)
			return -1;
	}

	/* Reset catalog and inventory state without touching the CPU filter. */
	memset(&cpu_catalog, 0, sizeof(cpu_catalog));
	memset(&cpu_inv, 0, sizeof(cpu_inv));
	pending_tracked_valid = 0;
	pending_tracked_count = 0;
	catalog_initialized = 1;
	inventory_initialized = 1;

	for (int i = 0; i < count; i++) {
		struct cpu_desc *desc = &cpu_catalog.cpus[i];

		desc->cpu_id = cpus[i].cpu_id;
		desc->present = cpus[i].present;
		desc->online = cpus[i].online;
		desc->package_id = cpus[i].package_id;
		desc->core_id = cpus[i].core_id;
		desc->numa_node = cpus[i].numa_node;
	}

	cpu_catalog.present_count = count;
	cpu_catalog.online_count = 0;
	cpu_catalog.tracked_count = 0;
	cpu_catalog.detected_present_count = count;
	cpu_catalog.detected_online_count = 0;
	for (int i = 0; i < count; i++) {
		if (cpu_catalog.cpus[i].online)
			cpu_catalog.detected_online_count++;
		if (catalog_cpu_is_tracked(&cpu_catalog.cpus[i]))
			cpu_catalog.tracked_count++;
	}
	cpu_catalog.online_count = cpu_catalog.detected_online_count;

	sort_present_cpus();
	inventory_build();
	return 0;
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
