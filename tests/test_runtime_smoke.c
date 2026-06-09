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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../armstat_cli.h"
#include "../formatter.h"
#include "../idle_backend.h"
#include "../cpu_inventory.h"
#include "../sysstat.h"

void reset_machine_state(void);

static void reset_test_state(void)
{
	reset_columns();
	clear_field_overrides();
	clear_idle_state_overrides();
	set_format(FORMAT_TEXT);
	set_quiet(0);
	set_summary_mode(0);
	set_default_summary_output(0);
	set_header_interval(0);
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
	apply_default_pmu_events(&opts);
	return opts;
}

static void seed_single_cpu_inventory(void)
{
	memset(&cpu_catalog, 0, sizeof(cpu_catalog));
	memset(&cpu_inv, 0, sizeof(cpu_inv));

	cpu_catalog.present_count = 1;
	cpu_catalog.online_count = 1;
	cpu_catalog.tracked_count = 1;
	cpu_catalog.cpus[0].cpu_id = 0;
	cpu_catalog.cpus[0].present = 1;
	cpu_catalog.cpus[0].online = 1;
	cpu_catalog.cpus[0].numa_node = 0;

	cpu_inv.present_count = 1;
	cpu_inv.online_count = 1;
	cpu_inv.tracked_count = 1;
	cpu_inv.present_cpus[0] = 0;
	cpu_inv.online_cpus[0] = 0;
	cpu_inv.tracked_cpus[0] = 0;
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
	freqs[0].min_freq = 1700000;
	freqs[0].max_freq = 2200000;
	freqs[0].boost = 1;
	snprintf(freqs[0].governor, sizeof(freqs[0].governor), "performance");

	raw->cpu_count = 1;
	raw->effective_cpu_count = 1;
	raw->freqs = freqs;
	raw->interval_delta_us = 1000000ULL;
	raw->uncore_freq_hz = 1600000000ULL;
	raw->numa_temp_count = 1;
	raw->numa_temps[0] = 45000;

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

	rec->interval = 1;
	rec->timestamp = 1774665600;
	rec->cpu_count = 1;
	rec->cpu_count_filtered = 1;
	rec->cpu_row_count = 1;
	rec->cpu_rows = cpu_rows;
	rec->raw = raw;
	rec->stats = stats;
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

	serialize_json(ctx->rec, ctx->rec->interval);
	close_format(ctx->rec->stats);
}

static void emit_text_record(void *arg)
{
	const struct serializer_args *ctx = arg;

	serialize_text(ctx->rec, ctx->rec->interval);
}

static void emit_close_format(void *arg)
{
	(void)arg;

	close_format(NULL);
}

static void emit_interval_header(void *arg)
{
	const double *interval = arg;

	print_interval_header(*interval);
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

	assert(parse_test_args_result(3, nan_argv, &opts) < 0);
	assert(parse_test_args_result(3, inf_argv, &opts) < 0);
	assert(parse_test_args_result(3, zero_argv, &opts) < 0);
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

static void test_pmu_event_parse_validation(void)
{
	struct armstat_options opts;
	char *unknown_argv[] = {"armstat", "-p", "cycles,not-real", NULL};
	char *too_many_argv[] = {
		"armstat", "-p",
		"cycles,cycles,cycles,cycles,cycles,cycles,cycles,cycles,"
		"cycles,cycles,cycles,cycles,cycles,cycles,cycles,cycles,cycles",
		NULL
	};
	char *valid_argv[] = {"armstat", "-p", "cycles,l3d-cache-refill,0x11", NULL};

	assert(parse_test_args_result(3, unknown_argv, &opts) < 0);
	assert(parse_test_args_result(3, too_many_argv, &opts) < 0);
	assert(parse_test_args_result(3, valid_argv, &opts) == 0);
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
	double interval = 1.0;
	char *output;
	char *quiet_argv[] = {"armstat", "-q", NULL};
	char *dump_argv[] = {"armstat", "-D", NULL};
	char *quiet_header_argv[] = {"armstat", "-q", "-N", "1", NULL};
	char *dump_header_argv[] = {"armstat", "-D", "-N", "1", NULL};

	assert(parse_test_args_result(2, quiet_argv, &opts) == 0);
	output = capture_stdout(emit_interval_header, &interval);
	assert(strcmp(output, "") == 0);
	free(output);

	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "CPU") == NULL);
	assert(strstr(output, "armstat") == NULL);
	free(output);

	assert(parse_test_args_result(2, dump_argv, &opts) == 0);
	output = capture_stdout(emit_interval_header, &interval);
	assert(strcmp(output, "") == 0);
	free(output);

	assert(parse_test_args_result(4, quiet_header_argv, &opts) == 0);
	output = capture_stdout(emit_interval_header, &interval);
	assert(strcmp(output, "") == 0);
	free(output);

	assert(parse_test_args_result(4, dump_header_argv, &opts) == 0);
	output = capture_stdout(emit_interval_header, &interval);
	assert(strcmp(output, "") == 0);
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
	args.rec = &rec;

	output = capture_stdout(emit_csv_mixed_scope, &args);
	assert(strstr(output, "summary.avg_freq") != NULL);
	assert(strstr(output, "summary.power") != NULL);
	assert(strstr(output, "cpu.freq") != NULL);
	assert(strstr(output, "Scope,CPU") != NULL);
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
	stats.package_count = 1;
	stats.packages[0].package_id = 0;
	stats.packages[0].cpu_count = 1;
	args.rec = &rec;

	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"schema_version\": 4") != NULL);
	assert(strstr(output, "\"summary\": {") != NULL);
	assert(strstr(output, "\"cpus\": [") == NULL);
	assert(strstr(output, "\"packages\": [") == NULL);
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
	stats.package_count = 1;
	stats.packages[0].package_id = 7;
	stats.packages[0].cpu_count = 1;
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
	assert(strstr(output, "\"package_id\"") != NULL);
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
	set_format(FORMAT_JSON);
	reset_machine_state();

	output = capture_stdout(emit_close_format, NULL);
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

/* ============================================================================
 * TEST COVERAGE: Multi-CPU CSV serializer
 * ============================================================================ */

static void seed_multi_cpu_inventory(void)
{
	memset(&cpu_catalog, 0, sizeof(cpu_catalog));
	memset(&cpu_inv, 0, sizeof(cpu_inv));

	cpu_catalog.present_count = 3;
	cpu_catalog.online_count = 3;
	cpu_catalog.tracked_count = 3;
	for (int i = 0; i < 3; i++) {
		cpu_catalog.cpus[i].cpu_id = i;
		cpu_catalog.cpus[i].present = 1;
		cpu_catalog.cpus[i].online = 1;
		cpu_catalog.cpus[i].numa_node = 0;
		cpu_catalog.cpus[i].package_id = 0;
		cpu_catalog.cpus[i].core_id = i;
	}

	cpu_inv.present_count = 3;
	cpu_inv.online_count = 3;
	cpu_inv.tracked_count = 3;
	for (int i = 0; i < 3; i++) {
		cpu_inv.present_cpus[i] = i;
		cpu_inv.online_cpus[i] = i;
		cpu_inv.tracked_cpus[i] = i;
	}
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
		freqs[i].min_freq = 1700000;
		freqs[i].max_freq = 2500000;
		freqs[i].boost = 1;
		snprintf(freqs[i].governor, sizeof(freqs[i].governor),
			 "performance");

		cpu_rows[i].cpu_idx = i;
		stats->per_cpu_idle[i] = 90.0 + i;
		stats->per_cpu_iowait[i] = 0.5;
		stats->per_cpu_ipc[i] = 1.0 + i * 0.1;
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
	rec->cpu_count = 3;
	rec->cpu_count_filtered = 3;
	rec->cpu_row_count = 3;
	rec->cpu_rows = cpu_rows;
	rec->raw = raw;
	rec->stats = stats;
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
	set_format(FORMAT_CSV);
	set_default_summary_output(1);
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
	set_format(FORMAT_CSV);
	set_summary_mode(1);
	set_default_summary_output(1);
	parse_column_option("freq,power", 1);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;

	output = capture_stdout(emit_csv_mixed_scope, &args);

	/* Header should contain metadata columns */
	assert(strstr(output, "schema_version") != NULL);
	assert(strstr(output, "interval") != NULL);
	assert(strstr(output, "timestamp") != NULL);

	/* Header should contain summary-scoped field labels */
	assert(strstr(output, "AvgFreq") != NULL);
	assert(strstr(output, "Power") != NULL);

	/* Data row should have the schema_version value */
	assert(strstr(output, "4,") != NULL);

	/* Should contain a summary row */
	assert(strstr(output, ",SUM,") != NULL);

	free(output);

	set_summary_mode(0);
}

int main(void)
{
	test_invalid_interval_args_fail();
	test_cpu_filter_parse_validation();
	test_pmu_event_parse_validation();
	test_quiet_modes_suppress_startup_header();
	test_list_counters_includes_full_pmu_catalog();
	test_schedstat_invalid_falls_back_to_procstat();
	test_parse_summary_all_keeps_base_groups_only();
	test_parse_all_ipc_enables_default_pmu_pair();
	test_parse_probe_and_busy_source();
	test_mixed_scope_csv_serializer_uses_scoped_headers();
	test_summary_json_serializer_emits_schema_and_summary_only();
	test_default_json_package_has_unique_package_key();
	test_empty_json_selection_has_no_dangling_comma();
	test_empty_json_stream_closes_as_empty_array();
	test_text_serializer_emits_column_headers_and_values();
	test_multi_cpu_csv_serializer_emits_all_cpu_rows();
	test_summary_csv_serializer_emits_metadata_and_summary_fields();
	return 0;
}
