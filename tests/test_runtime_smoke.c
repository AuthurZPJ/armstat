// SPDX-License-Identifier: GPL-2.0
/*
 * Small runtime/output smoke tests using synthetic records.
 *
 * These tests intentionally avoid real sysfs/proc dependencies while still
 * exercising:
 *   - CLI option interactions (-S/-a/-I/--probe/--busy-source)
 *   - JSON/CSV serializer behavior on current schema/version
 */

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "armstat_cli.h"
#include "cpuidle.h"
#include "formatter.h"
#include "formatter_section.h"
#include "idle_backend.h"
#include "cpu_inventory.h"
#include "cpufreq.h"
#include "formatter_fields.h"
#include "pmu.h"
#include "sample_cache.h"
#include "sysstat.h"
#include "topology.h"

static int scope_has_enabled_field(enum field_scope scope, const char *field_id)
{
	struct field_desc *fields[64];
	int count;

	get_enabled_fields(scope, fields, &count);
	for (int i = 0; i < count; i++) {
		if (fields[i]->id && strcmp(fields[i]->id, field_id) == 0)
			return 1;
	}
	return 0;
}

static void assert_csv_rows_are_rectangular(const char *csv)
{
	const char *line = csv;
	int expected_cells = -1;

	while (line && *line) {
		const char *p = line;
		int cells = 1;
		int quoted = 0;

		while (*p && *p != '\n') {
			if (*p == '"') {
				if (quoted && p[1] == '"')
					p++;
				else
					quoted = !quoted;
			} else if (*p == ',' && !quoted) {
				cells++;
			}
			p++;
		}
		assert(!quoted);
		if (expected_cells < 0)
			expected_cells = cells;
		else
			assert(cells == expected_cells);
		line = *p == '\n' ? p + 1 : NULL;
	}
}

static void reset_test_state(void)
{
	reset_columns();
	clear_field_overrides();
	clear_idle_state_overrides();
	set_text_quiet(0);
	set_section_summary_mode(0);
	set_section_default_summary_output(0);
	set_text_header_interval(0);
	enable_pmu(0);
	enable_ipc(0);
	set_busy_source_mode(BUSY_SOURCE_AUTO);
	set_cpu_inventory_filter(NULL);
	optind = 1;
}

static int parse_test_args_result(int argc, char **argv,
				  struct armstat_options *opts)
{
	*opts = default_options;
	reset_test_state();
	return parse_args(argc, argv, opts);
}

static struct armstat_options parse_test_args(int argc, char **argv)
{
	struct armstat_options opts;

	assert(parse_test_args_result(argc, argv, &opts) == 0);
	assert(apply_default_pmu_events(&opts) == 0);
	return opts;
}

static void seed_single_cpu_inventory(void)
{
	struct cpu_inventory_seed cpus[1] = {
		{0, 1, 1, 0, 0, 0},
	};

	cpu_inventory_seed(cpus, 1);
}

static void seed_two_cpu_inventory(void)
{
	struct cpu_inventory_seed cpus[2] = {
		{0, 1, 1, 0, 0, 0},
		{4, 1, 1, 0, 1, 0},
	};

	cpu_inventory_seed(cpus, 2);
}

static void test_hotplug_rebuild_refreshes_cpufreq_state(void)
{
	set_cpu_inventory_filter(NULL);
	enable_cpuidle(0);
	close_pmu_events();
	free_sample_cache();
	close_cpufreq();

	seed_single_cpu_inventory();
	assert(init_cpufreq() == 0);
	assert(init_sample_cache(1) == 0);
	assert(get_cpufreq_tracked_count() == 1);

	seed_two_cpu_inventory();
	assert(rebuild_hotplug_dependent_state() == 0);
	assert(get_cpufreq_tracked_count() == 2);

	free_sample_cache();
	close_cpufreq();
	close_topology();
}

static void make_synthetic_record(struct interval_record *rec,
				  struct sys_snapshot *raw,
				  struct interval_stats *stats,
				  struct cpu_row *cpu_rows,
				  struct cpu_freq_info *freqs)
{
	memset(rec, 0, sizeof(*rec));
	memset(raw, 0, sizeof(*raw));
	memset(stats, 0, sizeof(*stats));
	memset(cpu_rows, 0, sizeof(*cpu_rows));
	memset(freqs, 0, sizeof(*freqs));

	seed_single_cpu_inventory();

	freqs[0].cpu_id = 0;
	freqs[0].cur_freq = 2200000;
	freqs[0].cur_freq_valid = 1;
	freqs[0].min_freq = 1700000;
	freqs[0].min_freq_valid = 1;
	freqs[0].max_freq = 2200000;
	freqs[0].max_freq_valid = 1;
	freqs[0].boost = 1;
	snprintf(freqs[0].governor, sizeof(freqs[0].governor), "performance");

	raw->cpu_count = 1;
	raw->effective_cpu_count = 1;
	raw->freqs = freqs;
	raw->interval_delta_us = 1000000ULL;
	raw->uncore_freq_hz = 1600000000ULL;
	raw->uncore_freq_valid = 1;
	raw->numa_temp_count = 1;
	raw->numa_temps[0] = 45000;
	raw->numa_temp_valid_mask = 1U;

	stats->avg_mhz = 2200.0;
	stats->busy_percent = 1.0;
	stats->avg_idle_percent = 99.0;
	stats->avg_iowait_percent = 0.5;
	stats->avg_power_mw = 120000;
	stats->interval_energy_joules = 120.5;
	stats->mem_bw = 4096;
	stats->ctx_switches = 123;
	stats->interrupts = 456;
	stats->soft_interrupts = 789;
	stats->ipc = 1.75;
	stats->per_cpu_idle[0] = 98.0;
	stats->per_cpu_iowait[0] = 0.5;
	stats->per_cpu_ipc[0] = 1.25;

	cpu_rows[0].cpu_idx = 0;
	cpu_rows[0].freq = freqs[0];
	cpu_rows[0].idle_percent = stats->per_cpu_idle[0];
	cpu_rows[0].iowait_percent = stats->per_cpu_iowait[0];
	cpu_rows[0].busy_percent = 100.0 - stats->per_cpu_idle[0];
	cpu_rows[0].ipc = stats->per_cpu_ipc[0];
	cpu_rows[0].temp_c = raw->numa_temps[0] / 1000.0;

	rec->interval = 1;
	rec->timestamp = 1774665600;
	rec->timestamp_ns = 1774665600123456789ULL;
	rec->duration_us = raw->interval_delta_us;
	rec->cpu_count = 1;
	rec->cpu_count_filtered = 1;
	rec->cpu_row_count = 1;
	rec->cpu_rows = cpu_rows;
	rec->numa_temp_count = 1;
	rec->numa_temps[0] = raw->numa_temps[0];
	rec->numa_temp_valid_mask = 1U;
	rec->summary.avg_mhz = stats->avg_mhz;
	rec->summary.uncore_freq_mhz = raw->uncore_freq_hz / 1000000.0;
	rec->summary.busy_percent = stats->busy_percent;
	rec->summary.idle_percent = stats->avg_idle_percent;
	rec->summary.iowait_percent = stats->avg_iowait_percent;
	rec->summary.power_mw = stats->avg_power_mw;
	rec->summary.energy_joules = stats->interval_energy_joules;
	rec->summary.mem_bw = stats->mem_bw;
	rec->summary.ctx_switches = stats->ctx_switches;
	rec->summary.interrupts = stats->interrupts;
	rec->summary.soft_interrupts = stats->soft_interrupts;
	rec->summary.ipc = stats->ipc;
}

static char *capture_stdout(void (*emit)(void *), void *arg)
{
	FILE *tmp;
	long size;
	char *buf;
	int saved_stdout;

	fflush(stdout);
	tmp = tmpfile();
	assert(tmp != NULL);
	saved_stdout = dup(STDOUT_FILENO);
	assert(saved_stdout >= 0);
	assert(dup2(fileno(tmp), STDOUT_FILENO) >= 0);

	emit(arg);
	fflush(stdout);

	assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
	close(saved_stdout);

	assert(fseek(tmp, 0, SEEK_END) == 0);
	size = ftell(tmp);
	assert(size >= 0);
	assert(fseek(tmp, 0, SEEK_SET) == 0);

	buf = calloc((size_t)size + 1, 1);
	assert(buf != NULL);
	if (size > 0)
		assert(fread(buf, 1, (size_t)size, tmp) == (size_t)size);

	fclose(tmp);
	return buf;
}

struct serializer_args {
	const struct interval_record *rec;
};

static void emit_csv_mixed_scope(void *arg)
{
	const struct serializer_args *ctx = arg;

	serialize_csv(ctx->rec);
}

static void emit_json_record(void *arg)
{
	const struct serializer_args *ctx = arg;

	serialize_json(ctx->rec);
	close_machine_json();
}

static void emit_text_record(void *arg)
{
	const struct serializer_args *ctx = arg;

	serialize_text(ctx->rec);
}

static void emit_close_machine_json(void *arg)
{
	(void)arg;

	close_machine_json();
}

struct header_args {
	const struct armstat_options *opts;
	double interval;
};

static void emit_interval_header(void *arg)
{
	const struct header_args *ctx = arg;

	print_interval_header(ctx->opts, ctx->interval);
}

static void emit_list_counters(void *arg)
{
	(void)arg;

	list_counters();
}

static void test_invalid_interval_args_fail(void)
{
	struct armstat_options opts;
	char *nan_argv[] = {"armstat", "-i", "nan", NULL};
	char *inf_argv[] = {"armstat", "-i", "inf", NULL};
	char *zero_argv[] = {"armstat", "-i", "0", NULL};
	char *sub_microsecond_argv[] = {"armstat", "-i", "0.0000001", NULL};

	assert(parse_test_args_result(3, nan_argv, &opts) < 0);
	assert(parse_test_args_result(3, inf_argv, &opts) < 0);
	assert(parse_test_args_result(3, zero_argv, &opts) < 0);
	assert(parse_test_args_result(3, sub_microsecond_argv, &opts) < 0);
}

static void test_cpu_filter_parse_validation(void)
{
	struct armstat_options opts;
	char *bad_argv[] = {"armstat", "-c", "bad", NULL};
	char *reverse_argv[] = {"armstat", "-c", "3-1", NULL};
	char *empty_argv[] = {"armstat", "-c", "0,,2", NULL};
	char *valid_argv[] = {"armstat", "-c", "0,2-3", NULL};

	assert(parse_test_args_result(3, bad_argv, &opts) < 0);
	assert(parse_test_args_result(3, reverse_argv, &opts) < 0);
	assert(parse_test_args_result(3, empty_argv, &opts) < 0);
	assert(parse_test_args_result(3, valid_argv, &opts) == 0);
}

static void test_column_list_parse_validation(void)
{
	struct armstat_options opts;
	char *empty_middle[] = {"armstat", "-s", "freq,,idle", NULL};
	char *empty_trailing[] = {"armstat", "-H", "power,", NULL};

	assert(parse_test_args_result(3, empty_middle, &opts) < 0);
	assert(parse_test_args_result(3, empty_trailing, &opts) < 0);
}

static void test_pmu_event_parse_validation(void)
{
	struct armstat_options opts;
	unsigned long long config;
	int type;
	char *unknown_argv[] = {"armstat", "-p", "cycles,not-real", NULL};
	char *duplicate_argv[] = {"armstat", "-p", "cycles,cycles", NULL};
	char *too_many_argv[] = {
		"armstat", "-p",
		"cycles,instructions,cache-references,cache-misses,branches,"
		"branch-misses,mem-access,mem-read,mem-write,l1d-cache-refill,"
		"l1d-cache,l1i-cache-refill,l1i-cache,l2d-cache-refill,"
		"l2d-cache,l3d-cache-refill,l3d-cache",
		NULL
	};
	char *valid_argv[] = {"armstat", "-p", "cycles,l3d-cache-refill,0x11", NULL};
	char *ipc_overflow_argv[] = {
		"armstat", "-I", "-p",
		"cycles,cache-references,cache-misses,branches,branch-misses,"
		"mem-access,mem-read,mem-write,l1d-cache-refill,l1d-cache,"
		"l1i-cache-refill,l1i-cache,l2d-cache-refill,l2d-cache,"
		"l3d-cache-refill,l3d-cache",
		NULL
	};

	assert(parse_test_args_result(3, unknown_argv, &opts) < 0);
	assert(parse_test_args_result(3, duplicate_argv, &opts) < 0);
	assert(parse_test_args_result(3, too_many_argv, &opts) < 0);
	assert(parse_test_args_result(3, valid_argv, &opts) == 0);
	assert(parse_test_args_result(4, ipc_overflow_argv, &opts) == 0);
	assert(apply_default_pmu_events(&opts) < 0);

	/* Match Linux's architectural Arm PMUv3 event definitions. */
	assert(resolve_pmu_event("mem-access", &type, &config) == 0);
	assert(type == 4 && config == 0x13);
	assert(resolve_pmu_event("mem-read", &type, &config) == 0);
	assert(type == 4 && config == 0x06);
	assert(resolve_pmu_event("mem-write", &type, &config) == 0);
	assert(type == 4 && config == 0x07);
	assert(resolve_pmu_event("l1i-cache", &type, &config) == 0);
	assert(type == 4 && config == 0x14);
	assert(resolve_pmu_event("l2d-cache-refill", &type, &config) == 0);
	assert(type == 4 && config == 0x17);
	assert(resolve_pmu_event("l2d-cache", &type, &config) == 0);
	assert(type == 4 && config == 0x16);
	assert(resolve_pmu_event("l3d-cache-refill", &type, &config) == 0);
	assert(type == 4 && config == 0x2a);
	assert(resolve_pmu_event("l3d-cache", &type, &config) == 0);
	assert(type == 4 && config == 0x2b);
	assert(resolve_pmu_event("0X11", &type, &config) == 0);
	assert(type == 4 && config == 0x11);
	assert(resolve_pmu_event("0xffffffffffffffff", &type, &config) == 0);
	assert(config == ULLONG_MAX);
	assert(resolve_pmu_event("0x10000000000000000", &type, &config) < 0);
	assert(resolve_pmu_event("0x11junk", &type, &config) < 0);
}

static void test_quiet_modes_suppress_startup_header(void)
{
	struct armstat_options opts;
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	struct header_args hargs;
	char *output;
	char *quiet_argv[] = {"armstat", "-q", NULL};
	char *dump_argv[] = {"armstat", "-D", NULL};
	char *quiet_header_argv[] = {"armstat", "-q", "-N", "1", NULL};
	char *dump_header_argv[] = {"armstat", "-D", "-N", "1", NULL};
	char *quiet_dump_argv[] = {"armstat", "-D", "-q", NULL};

	hargs.opts = &opts;
	hargs.interval = 1.0;

	assert(parse_test_args_result(2, quiet_argv, &opts) == 0);
	output = capture_stdout(emit_interval_header, &hargs);
	assert(strcmp(output, "") == 0);
	free(output);

	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "CPU") == NULL);
	assert(strstr(output, "armstat") == NULL);
	free(output);

	assert(parse_test_args_result(2, dump_argv, &opts) == 0);
	assert(opts.dump_once == 1);
	assert(opts.quiet == 0);
	output = capture_stdout(emit_interval_header, &hargs);
	assert(strstr(output, "armstat - ARM Server Performance Monitor") != NULL);
	free(output);
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "CPU") != NULL);
	free(output);

	assert(parse_test_args_result(4, quiet_header_argv, &opts) == 0);
	output = capture_stdout(emit_interval_header, &hargs);
	assert(strcmp(output, "") == 0);
	free(output);

	assert(parse_test_args_result(4, dump_header_argv, &opts) == 0);
	output = capture_stdout(emit_interval_header, &hargs);
	assert(strstr(output, "armstat - ARM Server Performance Monitor") != NULL);
	free(output);

	assert(parse_test_args_result(3, quiet_dump_argv, &opts) == 0);
	output = capture_stdout(emit_interval_header, &hargs);
	assert(strcmp(output, "") == 0);
	free(output);
}

static void test_interval_header_preserves_subsecond_precision(void)
{
	struct armstat_options opts = default_options;
	struct header_args hargs = {
		.opts = &opts,
	};
	char *output;

	hargs.interval = 1.0;
	output = capture_stdout(emit_interval_header, &hargs);
	assert(strstr(output, "Sampling interval: 1 second(s)") != NULL);
	free(output);

	hargs.interval = 0.01;
	output = capture_stdout(emit_interval_header, &hargs);
	assert(strstr(output, "Sampling interval: 0.01 second(s)") != NULL);
	free(output);

	hargs.interval = 0.000001;
	output = capture_stdout(emit_interval_header, &hargs);
	assert(strstr(output, "Sampling interval: 0.000001 second(s)") != NULL);
	free(output);
}

static void test_list_counters_includes_full_pmu_catalog(void)
{
	char *output;

	reset_test_state();
	output = capture_stdout(emit_list_counters, NULL);
	assert(strstr(output, "l1d-cache-refill") != NULL);
	assert(strstr(output, "l2d-cache-refill") != NULL);
	assert(strstr(output, "l3d-cache-refill") != NULL);
	assert(strstr(output, "l3d-cache") != NULL);
	assert(strstr(output, "type=number") != NULL);
	assert(strstr(output, "unit=MHz") != NULL);
	assert(strstr(output, "unit=MiB/s") != NULL);
	assert(strstr(output, "type=boolean") != NULL);
	free(output);
}

static void test_schedstat_invalid_falls_back_to_procstat(void)
{
	struct sys_snapshot raw;
	struct interval_stats stats;
	unsigned long long idle_jiffies[1] = {0};
	unsigned long long iowait_jiffies[1] = {0};
	unsigned long long runtime_ns[1] = {0};
	unsigned char runtime_valid[1] = {0};
	int hz = get_kernel_hz();

	reset_test_state();
	seed_single_cpu_inventory();
	set_busy_source_mode(BUSY_SOURCE_SCHEDSTAT);
	init_aggregator();

	memset(&raw, 0, sizeof(raw));
	raw.cpu_count = 1;
	raw.effective_cpu_count = 1;
	raw.interval_delta_us = 0;
	raw.authoritative_idle_jiffies = idle_jiffies;
	raw.authoritative_iowait_jiffies = iowait_jiffies;
	raw.authoritative_runtime_ns = runtime_ns;
	raw.authoritative_runtime_valid = runtime_valid;
	calculate_interval_stats(&raw, &stats);

	idle_jiffies[0] = (unsigned long long)hz / 2ULL;
	raw.interval_delta_us = 1000000ULL;
	calculate_interval_stats(&raw, &stats);

	assert(stats.avg_idle_percent > 49.0);
	assert(stats.avg_idle_percent < 51.0);
	assert(stats.busy_percent > 49.0);
	assert(stats.busy_percent < 51.0);

	reset_aggregator();
	set_busy_source_mode(BUSY_SOURCE_AUTO);
}

static void test_parse_summary_all_keeps_base_groups_only(void)
{
	char *argv[] = {"armstat", "-S", "-a", NULL};
	struct armstat_options opts = parse_test_args(3, argv);

	assert(opts.summary_mode == 1);
	assert(show_freq == 1);
	assert(show_idle == 1);
	assert(show_power == 1);
	assert(show_temp == 1);
	assert(show_sysstat == 1);
	assert(show_membw == 1);
	assert(show_pmu == 0);
	assert(show_ipc == 0);
}

static void test_parse_all_ipc_enables_default_pmu_pair(void)
{
	char *argv[] = {"armstat", "-a", "-I", NULL};
	struct armstat_options opts = parse_test_args(3, argv);

	assert(show_pmu == 1);
	assert(show_ipc == 1);
	assert(opts.pmu_events != NULL);
	assert(strcmp(opts.pmu_events, "cycles,instructions") == 0);
}

static void test_independent_metric_options_are_order_independent(void)
{
	char *ipc_before[] = {"armstat", "-I", "-s", "freq", NULL};
	char *ipc_after[] = {"armstat", "-s", "freq", "-I", NULL};
	char *pmu_before[] = {"armstat", "-p", "cycles", "-s", "freq", NULL};
	char *energy_before[] = {"armstat", "-J", "-s", "freq", NULL};
	char *energy_after[] = {"armstat", "-s", "freq", "-J", NULL};

	parse_test_args(4, ipc_before);
	assert(show_pmu == 1);
	assert(show_ipc == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_ipc"));

	parse_test_args(4, ipc_after);
	assert(show_pmu == 1);
	assert(show_ipc == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_ipc"));

	parse_test_args(5, pmu_before);
	assert(show_pmu == 1);

	parse_test_args(4, energy_before);
	assert(show_energy == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "energy_joules"));

	parse_test_args(4, energy_after);
	assert(show_energy == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "energy_joules"));
}

static void test_repeated_show_options_form_a_union(void)
{
	char *argv[] = {"armstat", "-s", "freq", "-s", "idle", NULL};

	parse_test_args(5, argv);
	assert(show_freq == 1);
	assert(show_idle == 1);
	assert(any_fields_enabled(FIELD_SCOPE_CPU));
}

static void test_parse_probe_and_busy_source(void)
{
	char *argv[] = {"armstat", "--probe", "--busy-source", "task-clock", NULL};
	struct armstat_options opts = parse_test_args(4, argv);

	assert(opts.probe_only == 1);
	assert(get_busy_source_mode() == BUSY_SOURCE_TASK_CLOCK);
}

static void test_mixed_scope_csv_serializer_uses_scoped_headers(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;
	char *argv[] = {"armstat", "-f", "csv", "-s", "freq,power", NULL};

	parse_test_args(5, argv);
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.interval = 2147483648ULL;
	args.rec = &rec;

	output = capture_stdout(emit_csv_mixed_scope, &args);
	assert(strstr(output, "7,2147483648,1000000,") != NULL);
	assert(strstr(output, "summary.avg_freq") != NULL);
	assert(strstr(output, "summary.power") != NULL);
	assert(strstr(output, "cpu.freq") != NULL);
	assert(strstr(output, "Scope,CPU,Package") != NULL);
	free(output);
}

static void test_summary_json_serializer_emits_schema_and_summary_only(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;
	char *argv[] = {"armstat", "-S", "-a", "-f", "json", NULL};

	parse_test_args(5, argv);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.interval = 2147483648ULL;
	rec.package_count = 1;
	rec.packages[0].package_id = 0;
	rec.packages[0].cpu_count = 1;
	args.rec = &rec;

	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"schema_version\": 7") != NULL);
	assert(strstr(output, "\"interval\": 2147483648") != NULL);
	assert(strstr(output, "\"duration_us\": 1000000") != NULL);
	assert(strstr(output, "\"timestamp_ns\": 1774665600123456789") != NULL);
	assert(strstr(output, ".123456789+00:00") != NULL);
	assert(strstr(output, "\"ctx_switches\": 123") != NULL);
	assert(strstr(output, "\"ctx_switches\": 123.00") == NULL);
	assert(strstr(output, "\"summary\": {") != NULL);
	assert(strstr(output, "\"cpus\": [") == NULL);
	assert(strstr(output, "\"packages\": [") == NULL);
	free(output);
}

static void test_unavailable_sensor_values_are_not_fake_zeroes(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;
	char *argv[] = {"armstat", "-S", "-s", "power", "-f", "json", NULL};

	parse_test_args(6, argv);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.summary.power_mw = INFINITY;
	rec.summary.avg_mhz = NAN;
	rec.summary.uncore_freq_mhz = NAN;
	rec.numa_temp_valid_mask = 0;
	rec.cpu_rows[0].freq.cur_freq_valid = 0;
	rec.cpu_rows[0].freq.min_freq_valid = 0;
	rec.cpu_rows[0].freq.max_freq_valid = 0;
	rec.cpu_rows[0].temp_c = NAN;
	args.rec = &rec;

	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"power\": null") != NULL);
	free(output);

	assert(isnan(get_temp_vdie0(&rec, 0)));
	assert(isnan(get_cpu_temp_c(&rec, 0)));
	assert(isnan(get_cpu_freq_mhz(&rec, 0)));
	assert(isnan(get_cpu_min_freq_mhz(&rec, 0)));
	assert(isnan(get_cpu_max_freq_mhz(&rec, 0)));
	assert(isnan(get_summary_avg_mhz(&rec, 0)));
	assert(isnan(get_summary_uncore_freq_mhz(&rec, 0)));

	rec.numa_temp_count = 3;
	rec.numa_temp_valid_mask = 1U << 2;
	rec.numa_temps[2] = -500;
	assert(isnan(get_temp_vdie0(&rec, 0)));
	assert(get_temp_vdie2(&rec, 0) == -0.5);
}

static void test_machine_string_and_boolean_semantics(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	struct field_desc *governor;
	struct field_desc *boost;
	char value[32];
	char *output;
	char *argv[] = {"armstat", "-f", "json", "-s",
			"governor,boost", NULL};

	parse_test_args(5, argv);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;
	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"governor\": \"performance\"") != NULL);
	assert(strstr(output, "\"boost\": true") != NULL);
	free(output);

	rec.cpu_rows[0].freq.governor[0] = '\0';
	rec.cpu_rows[0].freq.boost = -1;
	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"governor\": null") != NULL);
	assert(strstr(output, "\"boost\": null") != NULL);
	free(output);

	governor = get_field_desc("governor");
	boost = get_field_desc("boost");
	assert(governor != NULL && boost != NULL);
	format_field_value(governor, &rec, 0, "", value, sizeof(value));
	assert(strcmp(value, "") == 0);
	format_field_value(boost, &rec, 0, "-", value, sizeof(value));
	assert(strcmp(value, "-") == 0);

	rec.cpu_rows[0].freq.boost = 0;
	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"boost\": false") != NULL);
	free(output);
}

static void test_default_json_package_has_unique_package_key(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;
	char *package_key;
	char *package_object_end;
	char *second_package_key;
	char *argv[] = {"armstat", "-f", "json", "-s", "pkg", NULL};

	parse_test_args(5, argv);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.package_count = 1;
	rec.packages[0].package_id = 7;
	rec.packages[0].cpu_count = 1;
	args.rec = &rec;

	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"packages\": [") != NULL);
	assert(strstr(output, "\"package\": 7") != NULL);
	package_key = strstr(output, "\"package\":");
	assert(package_key != NULL);
	package_object_end = strstr(package_key, "}");
	assert(package_object_end != NULL);
	second_package_key = strstr(package_key + strlen("\"package\":"),
				    "\"package\":");
	assert(second_package_key == NULL || second_package_key > package_object_end);
	assert(strstr(output, "\"package_id\"") == NULL);
	free(output);
}

static void test_empty_json_selection_has_no_dangling_comma(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;
	char *argv[] = {"armstat", "-f", "json", "-H", "all", NULL};

	parse_test_args(5, argv);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;

	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"cpus\": [") == NULL);
	assert(strstr(output, "\"summary\": {") == NULL);
	assert(strstr(output, ",\n  }") == NULL);
	free(output);
}

static void test_empty_json_stream_closes_as_empty_array(void)
{
	char *output;

	reset_test_state();
	reset_machine_state();

	output = capture_stdout(emit_close_machine_json, NULL);
	assert(strcmp(output, "[\n]\n") == 0);
	free(output);
}

/* ============================================================================
 * TEST COVERAGE: Text serializer content validation
 * ============================================================================ */

static void test_text_serializer_emits_column_headers_and_values(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;

	reset_test_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;

	output = capture_stdout(emit_text_record, &args);

	/* Header row should contain the selected column names */
	assert(strstr(output, "Freq") != NULL);
	assert(strstr(output, "Idle%") != NULL);
	assert(strstr(output, "Busy%") != NULL);

	/* Data row should contain the actual frequency value (2200 MHz) */
	assert(strstr(output, "2200") != NULL);

	free(output);
}

static void test_default_text_suppresses_package_rows(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;

	reset_test_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.package_count = 1;
	rec.packages[0].package_id = 0;
	rec.packages[0].cpu_count = 1;
	args.rec = &rec;

	/* Default mode: per-CPU rows only — no package aggregation rows. */
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "Pkg") == NULL);
	assert(strstr(output, "CPU") != NULL);
	free(output);

	/* -a enables the package and SUM sections alongside per-CPU rows. */
	set_all_columns_enabled(1);
	set_section_default_summary_output(1);
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "Pkg0") != NULL);
	assert(strstr(output, "SUM") != NULL);
	/* The SUM, Pkg, and CPU sections are separated by blank lines. */
	assert(strstr(output, "\n\n") != NULL);
	/* The package id is shown once, in the row key — the redundant
	 * pkg_id column is dropped from text output. */
	assert(strstr(output, "Pkg    Freq") != NULL);
	assert(strstr(output, "Pkg    Pkg") == NULL);
	free(output);
}

/* ============================================================================
 * TEST COVERAGE: Multi-CPU CSV serializer
 * ============================================================================ */

static void seed_multi_cpu_inventory(void)
{
	struct cpu_inventory_seed cpus[3] = {
		{0, 1, 1, 0, 0, 0},
		{1, 1, 1, 0, 1, 0},
		{2, 1, 1, 0, 2, 0},
	};

	cpu_inventory_seed(cpus, 3);
}

static void make_multi_cpu_synthetic_record(struct interval_record *rec,
					    struct sys_snapshot *raw,
					    struct interval_stats *stats,
					    struct cpu_row *cpu_rows,
					    struct cpu_freq_info *freqs)
{
	memset(rec, 0, sizeof(*rec));
	memset(raw, 0, sizeof(*raw));
	memset(stats, 0, sizeof(*stats));
	memset(cpu_rows, 0, 3 * sizeof(*cpu_rows));
	memset(freqs, 0, 3 * sizeof(*freqs));

	seed_multi_cpu_inventory();

	for (int i = 0; i < 3; i++) {
		freqs[i].cpu_id = i;
		freqs[i].cur_freq = (2000 + i * 100) * 1000;
		freqs[i].cur_freq_valid = 1;
		freqs[i].min_freq = 1700000;
		freqs[i].min_freq_valid = 1;
		freqs[i].max_freq = 2500000;
		freqs[i].max_freq_valid = 1;
		freqs[i].boost = 1;
		snprintf(freqs[i].governor, sizeof(freqs[i].governor),
			 "performance");

		cpu_rows[i].cpu_idx = i;
		stats->per_cpu_idle[i] = 90.0 + i;
		stats->per_cpu_iowait[i] = 0.5;
		stats->per_cpu_ipc[i] = 1.0 + i * 0.1;

		cpu_rows[i].freq = freqs[i];
		cpu_rows[i].idle_percent = stats->per_cpu_idle[i];
		cpu_rows[i].iowait_percent = stats->per_cpu_iowait[i];
		cpu_rows[i].busy_percent = 100.0 - stats->per_cpu_idle[i];
		cpu_rows[i].ipc = stats->per_cpu_ipc[i];
	}

	raw->cpu_count = 3;
	raw->effective_cpu_count = 3;
	raw->freqs = freqs;
	raw->interval_delta_us = 1000000ULL;
	raw->numa_temp_count = 0;

	stats->avg_mhz = 2100.0;
	stats->busy_percent = 10.0;
	stats->avg_idle_percent = 90.0;
	stats->avg_iowait_percent = 0.5;
	stats->per_cpu_idle[0] = 90.0;
	stats->per_cpu_idle[1] = 91.0;
	stats->per_cpu_idle[2] = 92.0;

	rec->interval = 1;
	rec->timestamp = 1774665600;
	rec->timestamp_ns = 1774665600123456789ULL;
	rec->duration_us = raw->interval_delta_us;
	rec->cpu_count = 3;
	rec->cpu_count_filtered = 3;
	rec->cpu_row_count = 3;
	rec->cpu_rows = cpu_rows;
	rec->summary.avg_mhz = stats->avg_mhz;
	rec->summary.busy_percent = stats->busy_percent;
	rec->summary.idle_percent = stats->avg_idle_percent;
	rec->summary.iowait_percent = stats->avg_iowait_percent;
}

static void test_multi_cpu_csv_serializer_emits_all_cpu_rows(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[3];
	struct cpu_freq_info freqs[3];
	struct serializer_args args;
	char *output;

	reset_test_state();
	set_section_default_summary_output(1);
	parse_column_option("freq,idle", 1);
	reset_machine_state();
	make_multi_cpu_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;

	output = capture_stdout(emit_csv_mixed_scope, &args);

	/* Header should contain Scope and CPU columns */
	assert(strstr(output, "Scope") != NULL);
	assert(strstr(output, "CPU") != NULL);
	assert(strstr(output, "schema_version") != NULL);

	/* Should contain data rows for all 3 CPUs */
	assert(strstr(output, ",CPU,0,") != NULL);
	assert(strstr(output, ",CPU,1,") != NULL);
	assert(strstr(output, ",CPU,2,") != NULL);

	/* Should contain a summary row */
	assert(strstr(output, ",SUM,") != NULL);
	assert_csv_rows_are_rectangular(output);

	free(output);
}

static void test_package_csv_serializer_emits_package_rows(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;
	char *argv[] = {"armstat", "-f", "csv", "-s", "pkg_avg_freq", NULL};

	parse_test_args(5, argv);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.package_count = 1;
	rec.packages[0].package_id = 7;
	rec.packages[0].cpu_count = 1;
	rec.packages[0].avg_mhz = 2134.5;
	args.rec = &rec;

	output = capture_stdout(emit_csv_mixed_scope, &args);
	assert(strstr(output, "Package,Freq") != NULL);
	assert(strstr(output, "7,1,1000000,") != NULL);
	assert(strstr(output, ",7,2134.50") != NULL);
	assert(strstr(output, "Scope") == NULL);
	assert_csv_rows_are_rectangular(output);
	free(output);
}

static void test_all_scope_csv_serializer_emits_package_rows(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;
	char *argv[] = {"armstat", "-a", "-f", "csv", NULL};

	parse_test_args(4, argv);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.package_count = 1;
	rec.packages[0].package_id = 3;
	rec.packages[0].cpu_count = 1;
	rec.packages[0].avg_mhz = 2200.0;
	rec.packages[0].idle_percent = 98.0;
	rec.packages[0].busy_percent = 2.0;
	rec.packages[0].iowait_percent = 0.5;
	args.rec = &rec;

	output = capture_stdout(emit_csv_mixed_scope, &args);
	assert(strstr(output, "Scope,CPU,Package") != NULL);
	assert(strstr(output, "package.freq") != NULL);
	assert(strstr(output, ",SUM,,,") != NULL);
	assert(strstr(output, ",PKG,,3,") != NULL);
	assert(strstr(output, ",CPU,0,,") != NULL);
	assert_csv_rows_are_rectangular(output);
	free(output);
}

/* ============================================================================
 * TEST COVERAGE: Summary CSV serializer (-S -f csv)
 * ============================================================================ */

static void test_summary_csv_serializer_emits_metadata_and_summary_fields(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;

	reset_test_state();
	set_section_summary_mode(1);
	set_section_default_summary_output(1);
	parse_column_option("freq,power", 1);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;

	output = capture_stdout(emit_csv_mixed_scope, &args);

	/* Header should contain metadata columns */
	assert(strstr(output, "schema_version") != NULL);
	assert(strstr(output, "interval") != NULL);
	assert(strstr(output, "timestamp") != NULL);
	assert(strstr(output, "timestamp_ns") != NULL);
	assert(strstr(output, "duration_us") != NULL);

	/* Header should contain summary-scoped field labels */
	assert(strstr(output, "AvgFreq") != NULL);
	assert(strstr(output, "Power") != NULL);

	/* Data row should have the schema_version value */
	assert(strstr(output, "7,") != NULL);
	assert(strstr(output, "timestamp_iso,Scope,") != NULL);

	/* Should contain a summary row */
	assert(strstr(output, ",SUM,") != NULL);

	free(output);

	set_section_summary_mode(0);
}

/* ============================================================================
 * TEST COVERAGE: interval_record materialization (owned values + LPI residual)
 * ============================================================================ */

/*
 * Build a real interval_record from a fake raw snapshot + interval stats and
 * verify that all per-interval dynamic values are copied into the record's own
 * storage (cpu_rows, packages, numa_temps, summary) instead of dangling
 * pointers into the raw data. Also checks the build-time LPI residual rule.
 */
static void test_interval_record_materializes_owned_values(void)
{
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_freq_info freqs[1];
	struct idle_state states[2];
	struct idle_state *idle_arr[1];
	struct interval_record *rec;

	reset_test_state();
	seed_single_cpu_inventory();

	memset(&raw, 0, sizeof(raw));
	memset(&stats, 0, sizeof(stats));
	memset(freqs, 0, sizeof(freqs));
	memset(states, 0, sizeof(states));

	freqs[0].cpu_id = 0;
	freqs[0].cur_freq = 2000000;
	freqs[0].cur_freq_valid = 1;
	freqs[0].min_freq = 1700000;
	freqs[0].min_freq_valid = 1;
	freqs[0].max_freq = 2500000;
	freqs[0].max_freq_valid = 1;
	freqs[0].boost = 1;
	snprintf(freqs[0].governor, sizeof(freqs[0].governor), "schedutil");

	/* Two usable idle states: state0 shallow (20%), state1 deep (residual). */
	states[0].available = 1;
	states[0].disabled = 0;
	states[0].percentage = 20.0;
	states[0].wakeups_per_sec = 100.0;
	states[1].available = 1;
	states[1].disabled = 0;
	states[1].percentage = 60.0;
	states[1].wakeups_per_sec = 10.0;
	idle_arr[0] = states;

	raw.cpu_count = 1;
	raw.effective_cpu_count = 1;
	raw.freqs = freqs;
	raw.idle = idle_arr;
	raw.idle_state_count = 2;
	raw.numa_temp_count = 1;
	raw.numa_temps[0] = 40000;
	raw.numa_temp_valid_mask = 1U;
	raw.interval_delta_us = 1000000ULL;
	raw.sample_timestamp = 1774665600;
	raw.sample_timestamp_ns = 1774665600123456789ULL;

	stats.per_cpu_idle[0] = 95.0;
	stats.per_cpu_iowait[0] = 0.5;
	stats.per_cpu_ipc[0] = 2.0;
	stats.avg_mhz = 2000.0;
	stats.busy_percent = 5.0;
	stats.avg_idle_percent = 95.0;
	stats.avg_iowait_percent = 0.5;
	stats.avg_power_mw = 100000;
	stats.interval_energy_joules = 100.0;
	stats.package_count = 1;
	stats.packages[0].package_id = 3;
	stats.packages[0].cpu_count = 1;
	stats.packages[0].avg_mhz = 2000.0;

	rec = build_interval_record(&raw, &stats, 1);
	assert(rec != NULL);
	assert(rec->timestamp == raw.sample_timestamp);
	assert(rec->timestamp_ns == raw.sample_timestamp_ns);
	assert(rec->duration_us == raw.interval_delta_us);

	/* Owned per-CPU freq is a copy, not a pointer into raw. */
	assert(rec->cpu_rows[0].freq.cur_freq == 2000000);
	assert(rec->cpu_rows[0].freq.min_freq == 1700000);
	assert(rec->cpu_rows[0].freq.max_freq == 2500000);
	assert(rec->cpu_rows[0].freq.boost == 1);
	assert(strcmp(rec->cpu_rows[0].freq.governor, "schedutil") == 0);

	/* Owned per-CPU busy/idle/iowait/ipc. */
	assert(rec->cpu_rows[0].idle_percent == 95.0);
	assert(rec->cpu_rows[0].busy_percent == 5.0);
	assert(rec->cpu_rows[0].iowait_percent == 0.5);
	assert(rec->cpu_rows[0].ipc == 2.0);

	/* Owned NUMA temps; CPU row temp derived from owning NUMA node. */
	assert(rec->numa_temp_count == 1);
	assert(rec->numa_temps[0] == 40000);
	assert(rec->cpu_rows[0].temp_c == 40.0);

	/* Owned package rows. */
	assert(rec->package_count == 1);
	assert(rec->packages[0].package_id == 3);
	assert(rec->packages[0].cpu_count == 1);
	assert(rec->packages[0].avg_mhz == 2000.0);

	/* Owned summary. */
	assert(rec->summary.avg_mhz == 2000.0);
	assert(rec->summary.busy_percent == 5.0);
	assert(rec->summary.idle_percent == 95.0);

	/*
	 * LPI residual (build-time): LPI-0 keeps the raw 20%, LPI-1 (deepest
	 * visible usable state) absorbs the rest so sum(LPI-*) == Idle% = 95.
	 * Hidden states materialize as NAN in CPU mode.
	 */
	assert(rec->cpu_rows[0].idle_state_pct[0] == 20.0);
	assert(rec->cpu_rows[0].idle_state_pct[1] == 75.0);
	assert(rec->cpu_rows[0].idle_state_pct[0] +
	       rec->cpu_rows[0].idle_state_pct[1] == 95.0);
	assert(isnan(rec->cpu_rows[0].idle_state_pct[2]));
	assert(isnan(rec->cpu_rows[0].idle_state_pct[3]));
	/* Wakeups owned per state; hidden states remain unavailable. */
	assert(rec->cpu_rows[0].idle_state_wakeups[0] == 100.0);
	assert(rec->cpu_rows[0].idle_state_wakeups[1] == 10.0);
	assert(isnan(rec->cpu_rows[0].idle_state_wakeups[2]));

	/*
	 * Summary idle-state residency is the average of per-CPU display values,
	 * computed at build time: single CPU -> same 20/75 split. Summary mode
	 * hides hidden states as 0.0, not NAN.
	 */
	assert(rec->summary_idle_state_pct[0] == 20.0);
	assert(rec->summary_idle_state_pct[1] == 75.0);
	assert(rec->summary_idle_state_pct[2] == 0.0);

	free_interval_record(rec);
}

int main(void)
{
	assert(setenv("TZ", "UTC0", 1) == 0);
	tzset();
	test_invalid_interval_args_fail();
	test_cpu_filter_parse_validation();
	test_column_list_parse_validation();
	test_pmu_event_parse_validation();
	test_quiet_modes_suppress_startup_header();
	test_interval_header_preserves_subsecond_precision();
	test_list_counters_includes_full_pmu_catalog();
	test_schedstat_invalid_falls_back_to_procstat();
	test_parse_summary_all_keeps_base_groups_only();
	test_parse_all_ipc_enables_default_pmu_pair();
	test_independent_metric_options_are_order_independent();
	test_repeated_show_options_form_a_union();
	test_parse_probe_and_busy_source();
	test_mixed_scope_csv_serializer_uses_scoped_headers();
	test_summary_json_serializer_emits_schema_and_summary_only();
	test_unavailable_sensor_values_are_not_fake_zeroes();
	test_machine_string_and_boolean_semantics();
	test_default_json_package_has_unique_package_key();
	test_empty_json_selection_has_no_dangling_comma();
	test_empty_json_stream_closes_as_empty_array();
	test_text_serializer_emits_column_headers_and_values();
	test_default_text_suppresses_package_rows();
	test_multi_cpu_csv_serializer_emits_all_cpu_rows();
	test_package_csv_serializer_emits_package_rows();
	test_all_scope_csv_serializer_emits_package_rows();
	test_summary_csv_serializer_emits_metadata_and_summary_fields();
	test_interval_record_materializes_owned_values();
	test_hotplug_rebuild_refreshes_cpufreq_state();
	return 0;
}
