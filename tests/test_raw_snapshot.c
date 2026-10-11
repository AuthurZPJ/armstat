// SPDX-License-Identifier: GPL-2.0
/* Raw snapshot tests use synthetic counters, never physical telemetry. */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "collector.h"
#include "cpu_inventory.h"
#include "formatter_raw.h"

struct raw_fixture {
	struct sys_snapshot snapshot;
	struct cpu_freq_info freqs[2];
	struct idle_state states[2][MAX_IDLE_STATES];
	struct idle_state *idle[2];
	unsigned long long idle_jiffies[2];
	unsigned long long iowait_jiffies[2];
	unsigned char procstat_valid[2];
	unsigned long long runtime_ns[2];
	unsigned char runtime_valid[2];
};

static void init_raw_fixture(struct raw_fixture *fixture)
{
	struct cpu_inventory_seed cpus[] = {
		{0, 1, 1, 0, 2, 0},
		{4, 1, 1, 1, 8, 2},
	};
	struct sys_snapshot *snapshot = &fixture->snapshot;

	memset(fixture, 0, sizeof(*fixture));
	assert(set_cpu_inventory_filter(NULL) == 0);
	assert(cpu_inventory_seed(cpus, 2) == 0);
	snapshot->cpu_count = 2;
	snapshot->effective_cpu_count = 2;
	snapshot->sample_timestamp_ns = 1791545322655761212ULL;
	snapshot->sample_monotonic_ns = 123456789ULL;
	snapshot->interval_delta_us = 1000000;
	snapshot->freqs = fixture->freqs;
	fixture->freqs[0] = (struct cpu_freq_info){
		.cpu_id = 17, /* Output identity must come from the tracked catalog. */
		.cur_freq = 2200000, .cur_freq_valid = 1,
		.min_freq = 0, .min_freq_valid = 1,
		.max_freq = 2500000, .max_freq_valid = 1,
		.boost = 0, .governor = "performance",
	};
	fixture->freqs[1] = (struct cpu_freq_info){
		.cpu_id = 19, .cur_freq = 777, .min_freq = 888, .max_freq = 999,
		.boost = -1,
	};
	snapshot->idle = fixture->idle;
	snapshot->idle_state_count = MAX_IDLE_STATES;
	fixture->idle[0] = fixture->states[0];
	fixture->idle[1] = fixture->states[1];
	fixture->states[0][0] = (struct idle_state){
		.time = 123456, .time_valid = 1, .usage = 0, .usage_valid = 1,
		.available = 1, .disabled = 0, .percentage = 57.25,
		.usage_per_sec = 912.5,
	};
	fixture->states[0][8] = (struct idle_state){
		.time = 98765, .usage = 5, .usage_valid = 1,
		.available = 1, .disabled = 1,
	};
	fixture->states[1][15] = (struct idle_state){
		.time = 0, .time_valid = 1, .usage = 0, .usage_valid = 1,
		.available = 1,
	};
	snapshot->authoritative_idle_jiffies = fixture->idle_jiffies;
	snapshot->authoritative_iowait_jiffies = fixture->iowait_jiffies;
	snapshot->authoritative_procstat_valid = fixture->procstat_valid;
	fixture->idle_jiffies[0] = 1000;
	fixture->iowait_jiffies[0] = 0;
	fixture->procstat_valid[0] = 1;
	fixture->idle_jiffies[1] = 777;
	fixture->iowait_jiffies[1] = 888;
	snapshot->authoritative_runtime_ns = fixture->runtime_ns;
	snapshot->authoritative_runtime_valid = fixture->runtime_valid;
	fixture->runtime_ns[0] = 0;
	fixture->runtime_valid[0] = 1;
	fixture->runtime_ns[1] = 999;
	snapshot->counters.ctx_switches = ULLONG_MAX;
	snapshot->counters.interrupts = 0;
	snapshot->counters.soft_interrupts = 123;
	snapshot->counters.sysstat_valid = 1;
	snapshot->counters.mem_bw_valid = 1;
	snapshot->counters.pmu_count = 1;
	snapshot->counters.pmu_valid = 1;
	snapshot->counters.pmu[0] = 4545454545ULL;
	snapshot->package_power_mw = 120000;
	snapshot->package_power_valid = 1;
	snapshot->uncore_freq_hz = 1600000000;
	snapshot->uncore_freq_valid = 1;
	snapshot->numa_temp_count = 3;
	snapshot->numa_temps[0] = 777;
	snapshot->numa_temps[2] = -500;
	snapshot->numa_temp_valid_mask = 1U << 2;
}

static char *capture_raw(const struct sys_snapshot *snapshot, int *result)
{
	FILE *file = tmpfile();
	int saved_stdout;
	long size;
	char *output;

	assert(file != NULL);
	assert(fflush(stdout) == 0);
	saved_stdout = dup(STDOUT_FILENO);
	assert(saved_stdout >= 0);
	assert(dup2(fileno(file), STDOUT_FILENO) >= 0);
	*result = serialize_raw_snapshot(snapshot);
	assert(fflush(stdout) == 0);
	assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
	close(saved_stdout);
	assert(fseek(file, 0, SEEK_END) == 0);
	size = ftell(file);
	assert(size >= 0);
	assert(fseek(file, 0, SEEK_SET) == 0);
	output = calloc((size_t)size + 1, 1);
	assert(output != NULL);
	if (size)
		assert(fread(output, 1, (size_t)size, file) == (size_t)size);
	fclose(file);
	return output;
}

static void table_row(const char *output, const char *section, const char *key,
		      char *row, size_t row_size)
{
	const char *start = strstr(output, section);
	const char *end;

	assert(start != NULL);
	start += strlen(section);
	end = strstr(start, "\n\n");
	while (*start && (!end || start < end)) {
		const char *next = strchr(start, '\n');
		char first[64];
		size_t len = next ? (size_t)(next - start) : strlen(start);

		assert(len < row_size);
		memcpy(row, start, len);
		row[len] = '\0';
		if (sscanf(row, "%63s", first) == 1 && strcmp(first, key) == 0)
			return;
		if (!next)
			break;
		start = next + 1;
	}
	assert(!"requested raw table row was not emitted");
}

static void test_units_cumulative_values_and_sparse_cpu_identity(void)
{
	struct raw_fixture fixture;
	char row[256], a[32], b[32], c[32], boost[32], governor[64];
	char *output;
	int cpu, package, core, node, result;

	init_raw_fixture(&fixture);
	output = capture_raw(&fixture.snapshot, &result);
	assert(result == 0);
	assert(strstr(output, "ctxt[count]: 18446744073709551615") != NULL);
	assert(strstr(output, "intr[count]: 0") != NULL);
	assert(strstr(output, "softirq[count]: 123") != NULL);
	assert(strstr(output, "Idle+IOWait[jiffies]") != NULL);
	table_row(output, "/proc/stat (cumulative", "0", row, sizeof(row));
	assert(sscanf(row, "%d %31s %31s", &cpu, a, b) == 3);
	assert(cpu == 0 && strcmp(a, "1000") == 0 && strcmp(b, "0") == 0);
	table_row(output, "/proc/stat (cumulative", "4", row, sizeof(row));
	assert(sscanf(row, "%d %31s %31s", &cpu, a, b) == 3);
	assert(cpu == 4 && strcmp(a, "-") == 0 && strcmp(b, "-") == 0);
	table_row(output, "/proc/schedstat (cumulative", "0", row, sizeof(row));
	assert(sscanf(row, "%d %31s", &cpu, a) == 2 && strcmp(a, "0") == 0);
	table_row(output, "/proc/schedstat (cumulative", "4", row, sizeof(row));
	assert(sscanf(row, "%d %31s", &cpu, a) == 2 && strcmp(a, "-") == 0);
	table_row(output, "CPU identity (", "4", row, sizeof(row));
	assert(sscanf(row, "%d %d %d %d", &cpu, &package, &core, &node) == 4);
	assert(cpu == 4 && package == 1 && core == 8 && node == 2);
	table_row(output, "CPU frequency (", "0", row, sizeof(row));
	assert(sscanf(row, "%d %31s %31s %31s %31s %63s",
		      &cpu, a, b, c, boost, governor) == 6);
	assert(cpu == 0 && strcmp(a, "2200000") == 0 && strcmp(b, "0") == 0);
	assert(strcmp(c, "2500000") == 0 && strcmp(boost, "0") == 0);
	assert(strcmp(governor, "performance") == 0);
	table_row(output, "CPU frequency (", "4", row, sizeof(row));
	assert(sscanf(row, "%d %31s %31s %31s %31s %63s",
		      &cpu, a, b, c, boost, governor) == 6);
	assert(cpu == 4 && strcmp(a, "-") == 0 && strcmp(b, "-") == 0);
	assert(strcmp(c, "-") == 0 && strcmp(boost, "-") == 0);
	assert(strcmp(governor, "-") == 0);
	assert(strstr(output, "memory_read[bytes]: 0") != NULL);
	assert(strstr(output, "package_power[mW]: 120000") != NULL);
	assert(strstr(output, "uncore_frequency[Hz]: 1600000000") != NULL);
	assert(strstr(output, "normalized from the hwmon source unit to mW") != NULL);
	table_row(output, "temperature_policy:", "2", row, sizeof(row));
	assert(sscanf(row, "%d %31s", &node, a) == 2 && strcmp(a, "-500") == 0);
	table_row(output, "temperature_policy:", "0", row, sizeof(row));
	assert(sscanf(row, "%d %31s", &node, a) == 2 && strcmp(a, "-") == 0);
	assert(strchr(output, '%') == NULL);
	assert(strstr(output, "PMU") == NULL && strstr(output, "Busy") == NULL);
	assert(strstr(output, "4545454545") == NULL);
	assert(strstr(output, "interval") == NULL && strstr(output, "57.25") == NULL);
	assert(strstr(output, "912.5") == NULL && strstr(output, "MiB/s") == NULL);
	free(output);
}

static void test_all_idle_states_preserve_independent_validity(void)
{
	struct raw_fixture fixture;
	const char *line;
	const char *end;
	char *output;
	int cpu, state, available, disabled, rows = 0, all_rows = 0, result;
	int found_usage_only = 0, found_valid_zero = 0;
	char time[32], usage[32];

	init_raw_fixture(&fixture);
	output = capture_raw(&fixture.snapshot, &result);
	assert(result == 0);
	assert(strstr(output, "DisableCached") != NULL);
	assert(strstr(output, "read validity is not exposed") != NULL);
	line = strstr(output, "CPU idle states (");
	assert(line != NULL);
	end = strstr(line, "\n\n");
	assert(end != NULL);
	while ((line = strchr(line, '\n')) != NULL && line < end) {
		line++;
		if (sscanf(line, "%d %d", &cpu, &state) == 2)
			all_rows++;
		if (sscanf(line, "%d %d %31s %31s %d %d",
			   &cpu, &state, time, usage, &available, &disabled) != 6)
			continue;
		rows++;
		assert(cpu == 0 || cpu == 4);
		assert(state >= 0 && state < MAX_IDLE_STATES);
		if (cpu == 0 && state == 0) {
			assert(strcmp(time, "123456") == 0 && strcmp(usage, "0") == 0);
			assert(available == 1 && disabled == 0);
		}
		if (cpu == 0 && state == 8) {
			assert(strcmp(time, "-") == 0 && strcmp(usage, "5") == 0);
			assert(available == 1 && disabled == 1);
			found_usage_only++;
		}
		if (cpu == 4 && state == 15) {
			assert(strcmp(time, "0") == 0 && strcmp(usage, "0") == 0);
			found_valid_zero++;
		}
	}
	/* Unavailable state rows have a '-' disable value, so parse separately. */
	assert(all_rows == 2 * MAX_IDLE_STATES);
	assert(rows == 3 && found_usage_only == 1 && found_valid_zero == 1);
	free(output);
}

static void test_missing_arrays_and_unavailable_sensor_values(void)
{
	struct raw_fixture fixture;
	char row[256], a[32], b[32], c[32], d[32], governor[64];
	char *output;
	int cpu, result;

	init_raw_fixture(&fixture);
	fixture.snapshot.freqs = NULL;
	fixture.snapshot.idle = NULL;
	fixture.snapshot.authoritative_idle_jiffies = NULL;
	fixture.snapshot.authoritative_iowait_jiffies = NULL;
	fixture.snapshot.authoritative_runtime_ns = NULL;
	fixture.snapshot.counters.sysstat_valid = 0;
	fixture.snapshot.counters.mem_bw_valid = 0;
	fixture.snapshot.package_power_valid = 0;
	fixture.snapshot.uncore_freq_valid = 0;
	fixture.snapshot.numa_temp_valid_mask = 0;
	output = capture_raw(&fixture.snapshot, &result);
	assert(result == 0);
	table_row(output, "CPU frequency (", "0", row, sizeof(row));
	assert(sscanf(row, "%d %31s %31s %31s %31s", &cpu, a, b, c, d) == 5);
	assert(strcmp(a, "-") == 0 && strcmp(b, "-") == 0);
	assert(strcmp(c, "-") == 0 && strcmp(d, "-") == 0);
	table_row(output, "/proc/stat (cumulative", "0", row, sizeof(row));
	assert(sscanf(row, "%d %31s %31s", &cpu, a, b) == 3);
	assert(strcmp(a, "-") == 0 && strcmp(b, "-") == 0);
	assert(strstr(output, "ctxt[count]: -") != NULL);
	assert(strstr(output, "memory_read[bytes]: -") != NULL);
	assert(strstr(output, "package_power[mW]: -") != NULL);
	assert(strstr(output, "uncore_frequency[Hz]: -") != NULL);
	free(output);
	init_raw_fixture(&fixture);
	fixture.idle[0] = NULL;
	memset(fixture.freqs[0].governor, 'x', sizeof(fixture.freqs[0].governor));
	output = capture_raw(&fixture.snapshot, &result);
	assert(result == 0);
	table_row(output, "CPU idle states (", "0", row, sizeof(row));
	assert(sscanf(row, "%d %31s %31s %31s %31s", &cpu, a, b, c, d) == 5);
	assert(strcmp(b, "-") == 0 && strcmp(c, "-") == 0 && strcmp(d, "-") == 0);
	assert(strstr(output, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx") != NULL);
	table_row(output, "CPU frequency (", "0", row, sizeof(row));
	assert(sscanf(row, "%d %31s %31s %31s %31s %63s",
		      &cpu, a, b, c, d, governor) == 6);
	assert(strlen(governor) == sizeof(fixture.freqs[0].governor));
	free(output);
}

static void test_invalid_snapshot_is_rejected_before_output(void)
{
	struct raw_fixture fixture;
	char *output;
	int result;

	init_raw_fixture(&fixture);
	output = capture_raw(NULL, &result);
	assert(result < 0 && errno == EINVAL && output[0] == '\0');
	free(output);
	for (int field = 0; field < 3; field++) {
		for (int negative = 0; negative < 2; negative++) {
			init_raw_fixture(&fixture);
			if (field == 0)
				fixture.snapshot.effective_cpu_count = negative ? -1 : MAX_CPUS + 1;
			else if (field == 1)
				fixture.snapshot.idle_state_count = negative ? -1 : MAX_IDLE_STATES + 1;
			else
				fixture.snapshot.numa_temp_count = negative ? -1 : 17;
			output = capture_raw(&fixture.snapshot, &result);
			assert(result < 0 && errno == EINVAL && output[0] == '\0');
			free(output);
		}
	}
}

static void test_stdout_write_failure_is_reported(void)
{
	struct raw_fixture fixture;
	int saved_stdout, read_only, sink, result;

	init_raw_fixture(&fixture);
	assert(fflush(stdout) == 0);
	saved_stdout = dup(STDOUT_FILENO);
	read_only = open("/dev/null", O_RDONLY);
	sink = open("/dev/null", O_WRONLY);
	assert(saved_stdout >= 0 && read_only >= 0 && sink >= 0);
	assert(dup2(read_only, STDOUT_FILENO) >= 0);
	result = serialize_raw_snapshot(&fixture.snapshot);
	assert(result < 0 && ferror(stdout));
	/* Discard any pending failed output before restoring the test's stdout. */
	assert(dup2(sink, STDOUT_FILENO) >= 0);
	clearerr(stdout);
	assert(fflush(stdout) == 0);
	assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
	close(saved_stdout);
	close(read_only);
	close(sink);
	clearerr(stdout);
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--raw-fixture") == 0) {
		struct raw_fixture fixture;

		init_raw_fixture(&fixture);
		return serialize_raw_snapshot(&fixture.snapshot) < 0;
	}
	assert(argc == 1);
	test_units_cumulative_values_and_sparse_cpu_identity();
	test_all_idle_states_preserve_independent_validity();
	test_missing_arrays_and_unavailable_sensor_values();
	test_invalid_snapshot_is_rejected_before_output();
	test_stdout_write_failure_is_reported();
	cleanup_cpu_inventory();
	printf("test_raw_snapshot: all tests passed\n");
	return 0;
}
