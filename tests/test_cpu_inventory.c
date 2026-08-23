// SPDX-License-Identifier: GPL-2.0
/*
 * Unit tests for the CPU identity module: sparse tracked<->real mapping,
 * the for_each_tracked_cpu view, and the sysfs path builder.
 *
 * These tests seed the inventory through the public cpu_inventory_seed() API
 * (as the other host tests do) so they run without /sys access. They validate
 * the identity contract, not the sysfs discovery code.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cpu_inventory.h"

/* Seed a sparse, non-contiguous tracked set: real ids 0, 4, 8, 12. */
static void seed_sparse_four(void)
{
	struct cpu_inventory_seed cpus[4] = {
		{0, 1, 1, 0, 0, 0},
		{4, 1, 1, 1, 0, 0},
		{8, 1, 1, 2, 0, 0},
		{12, 1, 1, 3, 0, 0},
	};

	cpu_inventory_seed(cpus, 4);
}

static void test_sparse_mapping(void)
{
	int expected[4] = {0, 4, 8, 12};

	seed_sparse_four();

	assert(get_tracked_cpu_count() == 4);
	for (int i = 0; i < 4; i++)
		assert(get_cpu_id_by_tracked_idx(i) == expected[i]);

	/* Negative and out-of-range indices are rejected, not wrapped. */
	assert(get_cpu_id_by_tracked_idx(-1) == -1);
	assert(get_cpu_id_by_tracked_idx(4) == -1);

	printf("  sparse mapping ok\n");
}

static void test_tracked_cpu_view(void)
{
	int expected[4] = {0, 4, 8, 12};
	int idx = -1;
	int count = 0;
	struct cpu_desc *desc;

	seed_sparse_four();

	for_each_tracked_cpu(idx, desc) {
		assert(desc != NULL);
		assert(desc->cpu_id == expected[idx]);
		assert(desc->present == 1);
		assert(desc->online == 1);
		assert(desc->package_id == idx);
		count++;
	}

	assert(count == 4);
	assert(idx == 4); /* loop leaves idx at tracked_count */

	printf("  tracked CPU view ok\n");
}

static void test_filtered_view(void)
{
	int expected[2] = {0, 8};
	int idx;
	int count = 0;
	struct cpu_desc *desc;

	/* Tracked set {0,4,8,12}; the filter keeps only 0 and 8. Seeding after
	 * activating the filter makes the derived tracked set match the filter. */
	set_cpu_inventory_filter("0,8");
	seed_sparse_four();

	assert(get_tracked_cpu_count() == 2);
	for_each_tracked_cpu(idx, desc) {
		assert(desc->cpu_id == expected[count]);
		count++;
	}
	assert(count == 2);

	set_cpu_inventory_filter(NULL);
}

static void test_sysfs_path(void)
{
	char buf[256];

	assert(cpu_sysfs_path(0, "cpufreq/cpuinfo_cur_freq", buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq") == 0);

	assert(cpu_sysfs_path(12, "cpuidle/state0/time", buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "/sys/devices/system/cpu/cpu12/cpuidle/state0/time") == 0);

	/* Empty subpath yields the cpu directory itself. */
	assert(cpu_sysfs_path(4, "", buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "/sys/devices/system/cpu/cpu4/") == 0);

	/* Truncation is reported, not silently accepted. */
	assert(cpu_sysfs_path(4, "cpufreq/cpuinfo_cur_freq", buf, 8) == -1);

	printf("  sysfs path builder ok\n");
}

static void test_cpu_list_mask_parser(void)
{
	unsigned char mask[MAX_CPUS];
	char long_list[8192];
	size_t used = 0;
	int count;
	int total_count;

	assert(parse_cpu_list_mask_with_total("0, 2-4,4,1020-2048\n", mask,
					      MAX_CPUS, &count,
					      &total_count) == 0);
	assert(count == 8);
	assert(total_count == 1033);
	assert(mask[0] && mask[2] && mask[3] && mask[4]);
	assert(mask[1020] && mask[1021] && mask[1022] && mask[1023]);
	assert(!mask[1] && !mask[5]);

	assert(parse_cpu_list_mask("4-2", mask, MAX_CPUS, &count) < 0);
	assert(parse_cpu_list_mask("1x", mask, MAX_CPUS, &count) < 0);
	assert(parse_cpu_list_mask("1,,2", mask, MAX_CPUS, &count) < 0);
	assert(parse_cpu_list_mask(" \n", mask, MAX_CPUS, &count) < 0);
	assert(parse_cpu_list_mask_with_total("0-10,5-15,7,20-22", mask,
					      MAX_CPUS, &count,
					      &total_count) == 0);
	assert(count == 19);
	assert(total_count == 19);

	/* A sparse 1024-CPU list exceeds the old 256-byte online-mask buffer. */
	for (int cpu = 0; cpu < MAX_CPUS; cpu += 2) {
		int written = snprintf(long_list + used, sizeof(long_list) - used,
				       "%s%d", used ? "," : "", cpu);

		assert(written > 0 && (size_t)written < sizeof(long_list) - used);
		used += (size_t)written;
	}
	assert(used > 256);
	assert(parse_cpu_list_mask(long_list, mask, MAX_CPUS, &count) == 0);
	assert(count == MAX_CPUS / 2);
	for (int cpu = 0; cpu < MAX_CPUS; cpu++)
		assert(mask[cpu] == (unsigned char)((cpu % 2) == 0));

	printf("  CPU list parser ok\n");
}

static void test_online_mask_catalog_match(void)
{
	unsigned char mask[MAX_CPUS] = {0};

	seed_sparse_four();
	mask[0] = 1;
	mask[4] = 1;
	mask[8] = 1;
	mask[12] = 1;
	assert(cpu_catalog_matches_online_mask(mask, MAX_CPUS, 4, 4));

	/* Same count but different membership must still force a full scan. */
	mask[12] = 0;
	mask[13] = 1;
	assert(!cpu_catalog_matches_online_mask(mask, MAX_CPUS, 4, 4));
	mask[13] = 0;
	mask[12] = 1;

	/* CPUs beyond the representable mask are detected through total_count. */
	assert(!cpu_catalog_matches_online_mask(mask, MAX_CPUS, 4, 5));
	assert(!cpu_catalog_matches_online_mask(mask, MAX_CPUS, 3, 4));

	printf("  online mask fast-path match ok\n");
}

int main(void)
{
	printf("test_cpu_inventory:\n");
	test_sparse_mapping();
	test_tracked_cpu_view();
	test_filtered_view();
	test_sysfs_path();
	test_cpu_list_mask_parser();
	test_online_mask_catalog_match();

	printf("test_cpu_inventory: all tests passed\n");
	return 0;
}
