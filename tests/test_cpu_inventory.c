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

#include "../cpu_inventory.h"

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

	assert(cpu_sysfs_path(0, "cpufreq/scaling_cur_freq", buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq") == 0);

	assert(cpu_sysfs_path(12, "cpuidle/state0/time", buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "/sys/devices/system/cpu/cpu12/cpuidle/state0/time") == 0);

	/* Empty subpath yields the cpu directory itself. */
	assert(cpu_sysfs_path(4, "", buf, sizeof(buf)) == 0);
	assert(strcmp(buf, "/sys/devices/system/cpu/cpu4/") == 0);

	/* Truncation is reported, not silently accepted. */
	assert(cpu_sysfs_path(4, "cpufreq/scaling_cur_freq", buf, 8) == -1);

	printf("  sysfs path builder ok\n");
}

int main(void)
{
	printf("test_cpu_inventory:\n");
	test_sparse_mapping();
	test_tracked_cpu_view();
	test_filtered_view();
	test_sysfs_path();

	printf("test_cpu_inventory: all tests passed\n");
	return 0;
}
