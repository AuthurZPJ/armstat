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

static int output_line_contains(const char *output, const char *line_key,
				const char *value)
{
	const char *line = output;

	while (line && *line) {
		const char *line_end = strchr(line, '\n');
		const char *key = strstr(line, line_key);
		const char *match = strstr(line, value);

		if (key && match && (!line_end || (key < line_end && match < line_end)))
			return 1;
		line = line_end ? line_end + 1 : NULL;
	}

	return 0;
}

static int count_output_matches(const char *output, const char *needle)
{
	int count = 0;

	for (const char *match = strstr(output, needle); match;
	     match = strstr(match + strlen(needle), needle))
		count++;
	return count;
}

static void reset_test_state(void)
{
	reset_columns();
	clear_field_overrides();
	clear_idle_state_overrides();
	set_text_quiet(0);
	set_section_summary_mode(0);
	set_section_default_summary_output(1);
	set_text_header_interval(0);
	enable_pmu(0);
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

	cpu_rows[0].tracked_idx = 0;
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

static char *capture_stream(FILE *stream, int stream_fd,
			    void (*emit)(void *), void *arg)
{
	FILE *tmp;
	long size;
	char *buf;
	int saved_stream;

	fflush(stream);
	tmp = tmpfile();
	assert(tmp != NULL);
	saved_stream = dup(stream_fd);
	assert(saved_stream >= 0);
	assert(dup2(fileno(tmp), stream_fd) >= 0);

	emit(arg);
	fflush(stream);

	assert(dup2(saved_stream, stream_fd) >= 0);
	close(saved_stream);

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

static char *capture_stdout(void (*emit)(void *), void *arg)
{
	return capture_stream(stdout, STDOUT_FILENO, emit, arg);
}

static char *capture_stderr(void (*emit)(void *), void *arg)
{
	return capture_stream(stderr, STDERR_FILENO, emit, arg);
}

struct serializer_args {
	const struct interval_record *rec;
};

static void emit_csv_mixed_scope(void *arg)
{
	const struct serializer_args *ctx = arg;

	assert(serialize_csv(ctx->rec) == 0);
}

static void emit_rejected_csv_record(void *arg)
{
	const struct serializer_args *ctx = arg;

	assert(serialize_csv(ctx->rec) < 0);
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

static void emit_two_text_records(void *arg)
{
	const struct serializer_args *ctx = arg;
	struct interval_record rec = *ctx->rec;

	rec.interval = 1;
	serialize_text(&rec);
	rec.interval = 2;
	serialize_text(&rec);
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
	assert(resolve_pmu_event("0x-1", &type, &config) < 0);
	assert(resolve_pmu_event("0x+11", &type, &config) < 0);
	assert(resolve_pmu_event("0x 11", &type, &config) < 0);
	assert(resolve_pmu_event("0x0x11", &type, &config) < 0);
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
	assert(strstr(output, "Freq[MHz]") == NULL);
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
	assert(strstr(output, "Freq[MHz]") != NULL);
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

static void test_default_busy_source_uses_procstat(void)
{
	struct sys_snapshot raw = {0};
	struct interval_stats stats;
	unsigned long long idle = 0, iowait = 0, runtime = 0;
	unsigned char runtime_valid = 1;

	reset_test_state();
	seed_single_cpu_inventory();
	set_busy_source_mode(BUSY_SOURCE_AUTO);
	assert(strcmp(get_busy_source_effective_name(), "procstat") == 0);
	for (int cpu = 0; cpu < MAX_CPUS; cpu++)
		assert(!busy_source_uses_schedstat_cpu(cpu));
	init_aggregator();
	raw.cpu_count = 1;
	raw.effective_cpu_count = 1;
	raw.authoritative_idle_jiffies = &idle;
	raw.authoritative_iowait_jiffies = &iowait;
	raw.authoritative_runtime_ns = &runtime;
	raw.authoritative_runtime_valid = &runtime_valid;
	calculate_interval_stats(&raw, &stats);

	/* A continuously running task has no new idle or settled runtime. */
	raw.interval_delta_us = 1000000ULL;
	calculate_interval_stats(&raw, &stats);
	assert(stats.per_cpu_busy[0] == 100.0);
	assert(stats.per_cpu_idle[0] == 0.0);

	/* Later schedstat settlement must not distort procstat's half-idle row. */
	idle = (unsigned long long)get_kernel_hz() / 2;
	runtime = 3000000000ULL;
	calculate_interval_stats(&raw, &stats);
	assert(stats.per_cpu_busy[0] > 49.0 && stats.per_cpu_busy[0] < 51.0);
	reset_aggregator();
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

static int synthetic_pmu_init_calls;
static int synthetic_pmu_init_result;
static int synthetic_pmu_init_diagnostics;
static char synthetic_pmu_init_events[512];

static int synthetic_pmu_init(const char *events)
{
	synthetic_pmu_init_calls++;
	synthetic_pmu_init_diagnostics = pmu_diagnostics_enabled();
	snprintf(synthetic_pmu_init_events, sizeof(synthetic_pmu_init_events),
		 "%s", events);
	return synthetic_pmu_init_result;
}

struct pmu_startup_args {
	const struct armstat_options *opts;
	int result;
};

static void emit_pmu_startup(void *arg)
{
	struct pmu_startup_args *ctx = arg;

	ctx->result = initialize_selected_pmu(ctx->opts, synthetic_pmu_init);
}

static char *capture_pmu_startup(const struct armstat_options *opts,
			       int init_result, int *startup_result)
{
	struct pmu_startup_args args = {opts, INT_MIN};
	char *output;

	synthetic_pmu_init_calls = 0;
	synthetic_pmu_init_result = init_result;
	synthetic_pmu_init_diagnostics = -1;
	synthetic_pmu_init_events[0] = '\0';
	output = capture_stderr(emit_pmu_startup, &args);
	*startup_result = args.result;
	return output;
}

static void test_automatic_ipc_success_preserves_selected_scopes(void)
{
	char *default_view[] = {"armstat", NULL};
	char *live[] = {"armstat", "-a", NULL};
	char *full[] = {"armstat", "-s", "all", NULL};
	char *cpu[] = {"armstat", "-s", "cpu", NULL};
	char **views[] = {default_view, live, full, cpu};
	int counts[] = {1, 2, 3, 3};

	for (int i = 0; i < ARRAY_SIZE(views); i++) {
		struct armstat_options opts = parse_test_args(counts[i], views[i]);
		int result;
		char *diagnostics;

		assert(opts.pmu_auto == 1);
		assert(strcmp(opts.pmu_events, "cycles,instructions") == 0);
		diagnostics = capture_pmu_startup(&opts, 0, &result);
		assert(result == 0 && synthetic_pmu_init_calls == 1);
		assert(synthetic_pmu_init_diagnostics == 0);
		assert(strcmp(synthetic_pmu_init_events, "cycles,instructions") == 0);
		assert(diagnostics[0] == '\0');
		assert(show_pmu == 0);
		assert(section_emit_cpu() == (i != 0));
		assert(section_emit_default_summary() == (i != 3));
		assert(section_emit_package() == (i == 2));
		assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc") == (i != 3));
		assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_ipc"));
		/* Closing counters later must not change a selected output layout. */
		close_pmu_events();
		assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc") == (i != 3));
		assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_ipc"));
		free(diagnostics);
	}
}

static void test_automatic_ipc_failure_hides_fields_and_clears_events(void)
{
	char *argv[] = {"armstat", "-a", NULL};
	struct armstat_options opts = parse_test_args(2, argv);
	int result;
	char *diagnostics;

	/* Parsed metadata exists even if this host cannot open perf counters. */
	seed_single_cpu_inventory();
	set_pmu_diagnostics(0);
	(void)init_pmu_events("cycles,instructions");
	assert(get_pmu_event_count() == 2);
	diagnostics = capture_pmu_startup(&opts, -1, &result);
	assert(result < 0 && synthetic_pmu_init_calls == 1);
	assert(synthetic_pmu_init_diagnostics == 0);
	assert(diagnostics[0] == '\0');
	assert(get_pmu_event_count() == 0);
	assert(rebuild_pmu_events() == 0);
	assert(!scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_ipc"));
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "busy_percent"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
	assert(section_emit_cpu() == 1 && show_pmu == 0);
	free(diagnostics);
}

static void test_unselected_ipc_does_not_initialize_perf(void)
{
	char *hidden[] = {"armstat", "-H", "ipc", NULL};
	char *frequency[] = {"armstat", "-s", "freq", NULL};
	char *hidden_cpu[] = {"armstat", "-s", "cpu", "-H", "ipc", NULL};
	char *hidden_summary_cpu[] = {"armstat", "-S", "-s", "cpu", NULL};
	char **views[] = {hidden, frequency, hidden_cpu, hidden_summary_cpu};
	int counts[] = {3, 3, 5, 4};

	for (int i = 0; i < ARRAY_SIZE(views); i++) {
		struct armstat_options opts = parse_test_args(counts[i], views[i]);
		int result;
		char *diagnostics = capture_pmu_startup(&opts, -1, &result);

		assert(opts.pmu_auto == 0 && opts.pmu_events == NULL);
		assert(result == 0 && synthetic_pmu_init_calls == 0);
		assert(diagnostics[0] == '\0');
		free(diagnostics);
	}
}

static void test_explicit_pmu_failure_keeps_requested_columns(void)
{
	char *ipc[] = {"armstat", "-I", NULL};
	char *ipc_group[] = {"armstat", "-s", "ipc", NULL};
	char *cpu_ipc[] = {"armstat", "-s", "cpu_ipc", NULL};
	char *custom[] = {"armstat", "-p", "cache-misses", NULL};
	char **views[] = {ipc, ipc_group, cpu_ipc, custom};
	int counts[] = {2, 3, 3, 3};

	for (int i = 0; i < ARRAY_SIZE(views); i++) {
		struct armstat_options opts = parse_test_args(counts[i], views[i]);
		struct interval_record rec;
		struct sys_snapshot raw;
		struct interval_stats stats;
		struct cpu_row cpu_rows[1];
		struct cpu_freq_info freqs[1];
		struct serializer_args args;
		int result;
		char *diagnostics = capture_pmu_startup(&opts, -1, &result);
		char *output;

		assert(opts.pmu_auto == 0);
		assert(result < 0 && synthetic_pmu_init_calls == 1);
		assert(synthetic_pmu_init_diagnostics == 1);
		assert(strstr(diagnostics, "Warning: PMU init failed") != NULL);
		assert(show_pmu == (i == 0 || i == 3));
		assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc") ==
		       (i == 0 || i == 1));
		assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_ipc") ==
		       (i != 3));
		assert(strcmp(opts.pmu_events, i == 3 ? "cache-misses" :
			      "cycles,instructions") == 0);
		reset_machine_state();
		make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
		args.rec = &rec;
		output = capture_stdout(emit_json_record, &args);
		assert((strstr(output, "\"ipc\": null") != NULL) == (i != 3));
		assert((strstr(output, "\"pmu\": null") != NULL) ==
		       (i == 0 || i == 3));
		free(output);
		free(diagnostics);
	}
}

static void test_explicit_ipc_event_merge_and_hide_order(void)
{
	char *hidden[] = {"armstat", "-I", "-H", "ipc", "-p", "cache-misses", NULL};
	char *spaced[] = {"armstat", "-I", "-p", " cycles , cache-misses ", NULL};
	char *ipc_spaced[] = {"armstat", "-s", "ipc", "-p", " instructions , cache-misses ", NULL};
	char *restore[] = {"armstat", "-H", "ipc", "-I", NULL};
	struct armstat_options opts = parse_test_args(6, hidden);

	assert(strcmp(opts.pmu_events, "cycles,instructions,cache-misses") == 0);
	assert(opts.pmu_auto == 0 && show_pmu == 1);
	assert(!scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc"));
	opts = parse_test_args(4, spaced);
	assert(strcmp(opts.pmu_events, "instructions, cycles , cache-misses ") == 0);
	opts = parse_test_args(5, ipc_spaced);
	assert(strcmp(opts.pmu_events, "cycles, instructions , cache-misses ") == 0);
	assert(show_pmu == 1 && opts.pmu_auto == 0);
	opts = parse_test_args(4, restore);
	assert(opts.ipc_requested == 1 && opts.pmu_auto == 0);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_ipc"));
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
	assert(show_membw == 0);
	assert(!scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "mem_bw"));
	assert(show_pmu == 0);
	assert(show_ipc == 1);
	assert(opts.pmu_auto == 1);
}

static void test_parse_all_ipc_enables_default_pmu_pair(void)
{
	char *argv[] = {"armstat", "-a", "-I", NULL};
	char *reverse[] = {"armstat", "-I", "-a", NULL};
	char *default_ipc[] = {"armstat", "-I", NULL};
	struct field_desc *expected_cpu[64], *expected_sum[64], *actual[64];
	int cpu_count, sum_count, actual_count;
	struct armstat_options opts = parse_test_args(3, argv);

	assert(show_pmu == 1);
	assert(show_ipc == 1);
	assert(opts.pmu_events != NULL);
	assert(strcmp(opts.pmu_events, "cycles,instructions") == 0);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "power_mw"));
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ctx_switches"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_busy_percent"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_ipc"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "governor"));
	assert(section_emit_package() == 0);
	get_enabled_fields(FIELD_SCOPE_CPU, expected_cpu, &cpu_count);
	get_enabled_fields(FIELD_SCOPE_SYSTEM, expected_sum, &sum_count);

	opts = parse_test_args(3, reverse);
	assert(strcmp(opts.pmu_events, "cycles,instructions") == 0);
	get_enabled_fields(FIELD_SCOPE_CPU, actual, &actual_count);
	assert(actual_count == cpu_count);
	assert(memcmp(actual, expected_cpu, cpu_count * sizeof(*actual)) == 0);
	get_enabled_fields(FIELD_SCOPE_SYSTEM, actual, &actual_count);
	assert(actual_count == sum_count);
	assert(memcmp(actual, expected_sum, sum_count * sizeof(*actual)) == 0);
	assert(section_emit_package() == 0);

	parse_test_args(2, default_ipc);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "summary_freq_mhz"));
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "power_mw"));
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "busy_percent"));
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "idle_percent"));
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_busy_percent"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_ipc"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "governor"));
	assert(show_sysstat == 0);
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

static void test_repeated_show_does_not_restore_hidden_independent_requests(void)
{
	char *ipc[] = {"armstat", "-s", "cpu", "-I", "-H", "ipc", "-s", "freq", NULL};
	char *pmu[] = {"armstat", "-s", "cpu", "-p", "cycles", "-H", "pmu", "-s", "freq", NULL};
	char *energy[] = {"armstat", "-s", "cpu", "-J", "-H", "energy", "-s", "freq", NULL};

	parse_test_args(8, ipc);
	assert(show_pmu == 1);
	assert(show_ipc == 0);
	assert(!scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_ipc"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));

	parse_test_args(9, pmu);
	assert(show_pmu == 0);
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));

	parse_test_args(8, energy);
	assert(show_energy == 0);
	assert(!scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "energy_joules"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
}

static void test_full_and_live_views_survive_repeated_show_options(void)
{
	char *full[] = {"armstat", "-s", "all", "-s", "freq", NULL};
	char *hide_config[] = {"armstat", "-s", "all", "-H", "freq-config", "-s", "idle", NULL};
	char *restore_freq[] = {"armstat", "-s", "all", "-H", "freq", "-s", "freq", NULL};
	char *live[] = {"armstat", "-s", "freq", "-a", "-s", "idle", NULL};

	parse_test_args(5, full);
	assert(section_emit_package() == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "power_mw"));
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ctx_switches"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "governor"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_busy_percent"));
	assert(show_idle_usage == 1);

	parse_test_args(7, hide_config);
	assert(section_emit_package() == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "power_mw"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_busy_percent"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "governor"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "min_freq_mhz"));

	parse_test_args(7, restore_freq);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "power_mw"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "governor"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_busy_percent"));

	parse_test_args(6, live);
	assert(section_emit_package() == 0);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ctx_switches"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "numa_node"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_busy_percent"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "governor"));
	assert(show_idle_usage == 0);
}

static void test_parse_probe_and_busy_source(void)
{
	char *argv[] = {"armstat", "--probe", "--busy-source", "task-clock", NULL};
	struct armstat_options opts = parse_test_args(4, argv);

	assert(opts.probe_only == 1);
	assert(get_busy_source_mode() == BUSY_SOURCE_TASK_CLOCK);
}

static void test_live_and_full_selection_have_distinct_data_surfaces(void)
{
	char *live[] = {"armstat", "-a", NULL};
	char *full[] = {"armstat", "-s", "ALL", NULL};
	char *core[] = {"armstat", "-s", "core", NULL};
	char *node[] = {"armstat", "-s", "numa", NULL};

	parse_test_args(2, live);
	assert(section_emit_cpu() == 1);
	assert(section_emit_default_summary() == 1);
	assert(section_emit_package() == 0);
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "package"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "core"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "numa_node"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "min_freq_mhz"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "governor"));
	assert(show_idle_usage == 0);
	assert(show_pmu == 0 && show_ipc == 1);

	parse_test_args(3, full);
	assert(section_emit_cpu() == 1);
	assert(section_emit_default_summary() == 1);
	assert(section_emit_package() == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "min_freq_mhz"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "governor"));
	assert(show_idle_usage == 1);
	assert(show_pmu == 0 && show_ipc == 1);

	parse_test_args(3, core);
	assert(section_emit_cpu() == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "core"));
	assert(section_has_output() == 1);

	parse_test_args(3, node);
	assert(section_emit_cpu() == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "numa_node"));
	assert(section_has_output() == 1);
}

static void test_cpu_shortcut_retains_hidden_fields_and_independent_requests(void)
{
	char *cpu[] = {"armstat", "-s", "cpu", NULL};
	char *hide_freq[] = {"armstat", "-s", "cpu", "-H", "freq", NULL};
	char *hide_idle[] = {"armstat", "-s", "cpu", "-H", "idle", NULL};
	char *energy[] = {"armstat", "-s", "cpu", "-J", NULL};
	char *ipc[] = {"armstat", "-s", "cpu", "-I", NULL};
	char *pmu[] = {"armstat", "-s", "cpu", "-p", "cycles", NULL};
	char *pmu_first[] = {"armstat", "-p", "cycles", "-s", "cpu", NULL};
	char *hide_ipc[] = {"armstat", "-s", "cpu", "-I", "-H", "ipc", NULL};
	char *expand[] = {"armstat", "-s", "cpu", "-a", NULL};
	char *narrow[] = {"armstat", "-a", "-s", "cpu", NULL};

	parse_test_args(3, cpu);
	assert(section_emit_cpu() == 1);
	assert(section_emit_default_summary() == 0);
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_busy_percent"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_iowait_percent"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "min_freq_mhz"));

	parse_test_args(5, hide_freq);
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_busy_percent"));
	assert(section_emit_default_summary() == 0);

	parse_test_args(5, hide_idle);
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "freq_mhz"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_busy_percent"));
	assert(!scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_iowait_percent"));
	assert(get_requested_series_mask(FIELD_SCOPE_CPU,
		FIELD_SERIES_IDLE_STATE_RESIDENCY) == 0U);

	parse_test_args(4, energy);
	assert(section_emit_default_summary() == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "energy_joules"));

	parse_test_args(4, ipc);
	assert(section_emit_default_summary() == 1);
	assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc"));

	parse_test_args(5, pmu);
	assert(section_emit_default_summary() == 1);
	assert(show_pmu == 1);

	parse_test_args(5, pmu_first);
	assert(section_emit_default_summary() == 1);
	assert(show_pmu == 1);

	parse_test_args(6, hide_ipc);
	assert(section_emit_default_summary() == 1);
	assert(show_pmu == 1);
	assert(!scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "ipc"));

	parse_test_args(4, expand);
	assert(section_emit_default_summary() == 1);
	assert(show_sysstat == 1);
	assert(section_emit_package() == 0);

	parse_test_args(4, narrow);
	assert(section_emit_default_summary() == 0);
	assert(scope_has_enabled_field(FIELD_SCOPE_CPU, "cpu_busy_percent"));
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
	char *argv[] = {"armstat", "-f", "csv", "-s", "cpu,freq,power", NULL};

	parse_test_args(5, argv);
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.interval = 2147483648ULL;
	args.rec = &rec;

	output = capture_stdout(emit_csv_mixed_scope, &args);
	assert(strstr(output, "9,2147483648,1000000,") != NULL);
	assert(strstr(output, "summary.freq") != NULL);
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
	assert(strstr(output, "\"schema_version\": 9") != NULL);
	assert(strstr(output, "\"interval\": 2147483648") != NULL);
	assert(strstr(output, "\"duration_us\": 1000000") != NULL);
	assert(strstr(output, "\"timestamp_ns\": 1774665600123456789") != NULL);
	assert(strstr(output, ".123456789+00:00") != NULL);
	assert(strstr(output, "\"ctx_switches\": 123") != NULL);
	assert(strstr(output, "\"ctx_switches\": 123.00") == NULL);
	assert(strstr(output, "\"summary\": {") != NULL);
	assert(strstr(output, "\"ipc\": null") != NULL);
	assert(strstr(output, "\"pmu\"") == NULL);
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

static void test_default_machine_output_emits_summary_only(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;

	reset_test_state();
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.package_count = 1;
	rec.packages[0].package_id = 7;
	rec.packages[0].cpu_count = 1;
	args.rec = &rec;

	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"schema_version\": 9") != NULL);
	assert(strstr(output, "\"summary\": {") != NULL);
	assert(strstr(output, "\"packages\": [") == NULL);
	assert(strstr(output, "\"cpus\": [") == NULL);
	free(output);

	output = capture_stdout(emit_csv_mixed_scope, &args);
	assert(strstr(output, "timestamp_iso,Scope,freq") != NULL);
	assert(strstr(output, ",SUM,") != NULL);
	assert(strstr(output, "Package") == NULL);
	assert(strstr(output, "CPU") == NULL);
	assert(strstr(output, "summary.") == NULL);
	assert(strstr(output, ",ipc") != NULL);
	assert(strstr(output, "pmu.") == NULL);
	assert(strstr(output, ",PKG,") == NULL);
	assert_csv_rows_are_rectangular(output);
	free(output);
}

static void test_default_pmu_cpu_output_retains_package_topology(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct cpu_inventory_seed cpus[1] = {
		{0, 1, 1, 7, 0, 0},
	};
	struct serializer_args args;
	char *output;
	char *argv[] = {"armstat", "-I", "-f", "json", NULL};

	parse_test_args(4, argv);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	assert(cpu_inventory_seed(cpus, 1) == 0);
	rec.package_count = 1;
	rec.packages[0].package_id = 7;
	args.rec = &rec;

	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"summary\": {") != NULL);
	assert(strstr(output, "\"cpus\": [") != NULL);
	assert(strstr(output, "\"cpu\": 0, \"package\": 7") != NULL);
	assert(strstr(output, "\"packages\": [") == NULL);
	free(output);

	/* Explicit hiding still removes topology without removing CPU rows. */
	assert(parse_column_option("pkg", 0) == 0);
	reset_machine_state();
	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"cpus\": [") != NULL);
	assert(strstr(output, "\"package\":") == NULL);
	assert(strstr(output, "\"packages\": [") == NULL);
	free(output);
}

static void test_selected_json_package_has_unique_package_key(void)
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
	assert(strstr(output, "Freq[MHz]") != NULL);
	assert(strstr(output, "Power[mW]") != NULL);
	assert(strstr(output, "Idle%") != NULL);
	assert(strstr(output, "Busy%") != NULL);

	/* Data row should contain the actual frequency value (2200 MHz) */
	assert(strstr(output, "2200") != NULL);

	free(output);
}

static void test_text_header_interval_counts_complete_data_rows(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;

	reset_test_state();
	set_text_header_interval(2);
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;

	rec.interval = 1;
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "Freq") != NULL);
	free(output);

	rec.interval = 2;
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "Freq") == NULL);
	free(output);

	rec.interval = 3;
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "Freq") != NULL);
	free(output);
	set_text_header_interval(0);
}

static void test_compact_pmu_only_summary_headers_quiet_and_refresh(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *argv[] = {"armstat", "-S", "-s", "pmu", "-p", "cycles", NULL};
	char *output;

	parse_test_args(6, argv);
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;
	/* Event metadata survives unavailable perf access on the host/VM. */
	(void)init_pmu_events("cycles");
	assert(get_pmu_event_count() == 1);
	assert(!any_fields_enabled(FIELD_SCOPE_SYSTEM));
	output = capture_stdout(emit_two_text_records, &args);
	assert(count_output_matches(output, "Timestamp") == 1);
	assert(count_output_matches(output, "cycles[/int]") == 1);
	assert(count_output_matches(output, "\n") == 3);
	assert(strstr(output, "--- interval") == NULL);
	assert(strstr(output, "\n\n") == NULL);
	free(output);

	set_text_quiet(0);
	set_text_header_interval(1);
	output = capture_stdout(emit_two_text_records, &args);
	assert(count_output_matches(output, "cycles[/int]") == 2);
	assert(count_output_matches(output, "\n") == 4);
	free(output);

	set_text_quiet(1);
	output = capture_stdout(emit_two_text_records, &args);
	assert(strstr(output, "Timestamp") == NULL);
	assert(strstr(output, "cycles[/int]") == NULL);
	assert(strstr(output, ".123+00:00") == NULL);
	assert(count_output_matches(output, "\n") == 2);
	free(output);
	close_pmu_events();
	reset_test_state();
}

static void test_changed_columns_reprint_text_and_reject_csv(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;

	reset_test_state();
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	set_section_summary_mode(1);
	args.rec = &rec;
	output = capture_stdout(emit_text_record, &args);
	free(output);
	output = capture_stdout(emit_csv_mixed_scope, &args);
	assert(strstr(output, "freq") != NULL);
	free(output);

	rec.interval = 2;
	/* Simulate a capability-dependent column disappearing after rebuild. */
	enable_freq(0);
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "Power[mW]") != NULL);
	assert(strstr(output, "Freq[MHz]") == NULL);
	free(output);
	output = capture_stdout(emit_rejected_csv_record, &args);
	assert(output[0] == '\0');
	free(output);

	/* A new stream can establish a different, internally consistent header. */
	reset_machine_state();
	output = capture_stdout(emit_csv_mixed_scope, &args);
	assert(strstr(output, "schema_version") != NULL);
	assert(strstr(output, "freq") == NULL);
	assert_csv_rows_are_rectangular(output);
	free(output);
}

static void test_default_live_and_full_text_views(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;
	char *live_argv[] = {"armstat", "-a", NULL};
	char *full_argv[] = {"armstat", "-s", "all", NULL};

	reset_test_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.package_count = 1;
	rec.packages[0].package_id = 0;
	rec.packages[0].cpu_count = 1;
	args.rec = &rec;

	/* Default mode: SUM only, even when package data is available. */
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "Pkg0") == NULL);
	assert(strstr(output, "SUM") != NULL);
	assert(section_emit_cpu() == 0);
	free(output);

	/* The daily CPU view keeps dynamic data and topology without duplicates. */
	parse_test_args(2, live_argv);
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "Pkg0") == NULL);
	assert(strstr(output, "CPU") != NULL);
	assert(strstr(output, "SUM") != NULL);
	assert(strstr(output, "Freq[MHz]") != NULL);
	assert(strstr(output, "Min[MHz]") == NULL);
	assert(strstr(output, "Governor") == NULL);
	assert(strstr(output, "_usage") == NULL);
	free(output);

	/* The explicit full view preserves configuration and package sections. */
	parse_test_args(3, full_argv);
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "Pkg0") != NULL);
	assert(strstr(output, "SUM") != NULL);
	assert(strstr(output, "Min[MHz]") != NULL);
	assert(strstr(output, "Governor") != NULL);
	assert(section_emit_cpu() == 1);
	/* The SUM, Pkg, and CPU sections are separated by blank lines. */
	assert(strstr(output, "\n\n") != NULL);
	/* The package id is shown once, in the row key — the redundant
	 * pkg_id column is dropped from text output. */
	assert(strstr(output, "Pkg    Freq") != NULL);
	assert(strstr(output, "Pkg    Pkg") == NULL);
	free(output);
}

static void test_membw_opt_in_survives_selection_order_and_serialization(void)
{
	char *default_view[] = {"armstat", NULL};
	char *summary[] = {"armstat", "-S", NULL};
	char *live[] = {"armstat", "-a", NULL};
	char *full[] = {"armstat", "-s", "all", NULL};
	char *summary_full[] = {"armstat", "-S", "-s", "all", NULL};
	char *group[] = {"armstat", "-s", "membw", NULL};
	char *alias[] = {"armstat", "-s", "mem", NULL};
	char *field[] = {"armstat", "-s", "mem_bw", NULL};
	char *full_group[] = {"armstat", "-s", "all,membw", NULL};
	char *group_full[] = {"armstat", "-s", "membw,all", NULL};
	char *full_then_group[] = {"armstat", "-s", "all", "-s", "membw", NULL};
	char *group_then_full[] = {"armstat", "-s", "membw", "-s", "all", NULL};
	char *hidden[] = {"armstat", "-s", "all,membw", "-H", "membw", NULL};
	char *hidden_then_full[] = {"armstat", "-s", "membw", "-H", "membw", "-s", "all", NULL};
	char *hidden_then_freq[] = {"armstat", "-s", "all,membw", "-H", "membw", "-s", "freq", NULL};
	char *explicit_then_live[] = {"armstat", "-s", "mem_bw", "-a", NULL};
	char *hidden_then_live[] = {"armstat", "-s", "membw", "-H", "membw", "-a", NULL};
	struct {
		char **argv;
		int argc;
		int membw;
		int automatic_ipc;
	} cases[] = {
		{default_view, 1, 0, 1},
		{summary, 2, 0, 1},
		{live, 2, 0, 1},
		{full, 3, 0, 1},
		{summary_full, 4, 0, 1},
		{group, 3, 1, 0},
		{alias, 3, 1, 0},
		{field, 3, 1, 0},
		{full_group, 3, 1, 1},
		{group_full, 3, 1, 1},
		{full_then_group, 5, 1, 1},
		{group_then_full, 5, 1, 1},
		{hidden, 5, 0, 1},
		{hidden_then_full, 7, 0, 1},
		{hidden_then_freq, 7, 0, 1},
		{explicit_then_live, 4, 1, 1},
		{hidden_then_live, 6, 0, 1},
	};
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;

	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.package_count = 1;
	rec.packages[0].package_id = 0;
	rec.packages[0].cpu_count = 1;
	args.rec = &rec;

	for (int i = 0; i < ARRAY_SIZE(cases); i++) {
		struct armstat_options opts = parse_test_args(cases[i].argc,
							    cases[i].argv);
		char *output;

		assert(show_membw == cases[i].membw);
		assert(scope_has_enabled_field(FIELD_SCOPE_SYSTEM, "mem_bw") ==
		       cases[i].membw);
		assert(opts.pmu_auto == cases[i].automatic_ipc);
		assert(show_pmu == 0);
		if (cases[i].automatic_ipc)
			assert(strcmp(opts.pmu_events, "cycles,instructions") == 0);
		else
			assert(opts.pmu_events == NULL);

		reset_machine_state();
		output = capture_stdout(emit_json_record, &args);
		assert((strstr(output, "\"mem_bw\":") != NULL) == cases[i].membw);
		if (cases[i].membw)
			assert(strstr(output, "\"mem_bw\": 4096.00") != NULL);
		free(output);

		reset_machine_state();
		output = capture_stdout(emit_csv_mixed_scope, &args);
		assert((strstr(output, "mem_bw") != NULL) == cases[i].membw);
		if (cases[i].membw)
			assert(strstr(output, "4096.00") != NULL);
		assert_csv_rows_are_rectangular(output);
		free(output);

		output = capture_stdout(emit_text_record, &args);
		assert((strstr(output, "MemBW[MiB/s]") != NULL) == cases[i].membw);
		if (cases[i].membw)
			assert(strstr(output, "4096.00") != NULL);
		free(output);
	}
}

static void test_text_package_rows_align_large_sparse_ids(void)
{
	struct interval_record rec;
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_row cpu_rows[1];
	struct cpu_freq_info freqs[1];
	struct serializer_args args;
	char *output;
	char *small_row;
	char *large_row;

	reset_test_state();
	enable_package(1);
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.package_count = 2;
	rec.packages[0].package_id = 0;
	rec.packages[0].avg_mhz = 2200;
	rec.packages[1].package_id = 10000;
	rec.packages[1].avg_mhz = 2400;
	args.rec = &rec;

	output = capture_stdout(emit_text_record, &args);
	small_row = strstr(output, "\nPkg0 ");
	large_row = strstr(output, "\nPkg10000 ");
	assert(small_row != NULL && large_row != NULL);
	assert(strstr(small_row, "2200") != NULL);
	assert(strstr(large_row, "2400") != NULL);
	assert(strstr(small_row, "2200") - small_row ==
	       strstr(large_row, "2400") - large_row);
	assert(strstr(output, "\nPkg       Freq[MHz]") != NULL);
	free(output);

	/* A wider package identity after hotplug also needs a fresh header. */
	rec.interval = 2;
	rec.packages[1].package_id = 100000;
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "\nPkg        Freq[MHz]") != NULL);
	free(output);
}

static void test_text_sample_blocks_have_visible_interval_boundaries(void)
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

	output = capture_stdout(emit_two_text_records, &args);
	assert(strstr(output, "--- interval") == NULL);
	assert(strstr(output, "\n\n") == NULL);
	assert(count_output_matches(output, "Timestamp") == 1);
	assert(count_output_matches(output, "\n") == 3);
	assert(strstr(output, ".123+00:00") != NULL);
	assert(strstr(output, ".123456789") == NULL);
	free(output);

	/* Multiple rows need a boundary identifying their complete interval. */
	enable_cpu(1);
	set_text_quiet(0);
	output = capture_stdout(emit_two_text_records, &args);
	assert(strstr(output, "--- interval 1 | ") != NULL);
	assert(strstr(output, "\n\n--- interval 2 | ") != NULL);
	assert(strstr(output, "duration 1.000000 s ---\n") != NULL);
	assert(strstr(output, ".123+00:00") != NULL);
	assert(strstr(output, ".123456789") == NULL);
	free(output);

	set_text_quiet(1);
	output = capture_stdout(emit_two_text_records, &args);
	assert(strstr(output, "--- interval") == NULL);
	assert(strstr(output, ".123456789") == NULL);
	free(output);

	set_text_quiet(0);
	set_section_summary_mode(1);
	output = capture_stdout(emit_two_text_records, &args);
	assert(strstr(output, "--- interval") == NULL);
	assert(strstr(output, "Timestamp") != NULL);
	assert(strstr(output, ".123+00:00") != NULL);
	free(output);

	/* Machine timestamps retain the independent nanosecond contract. */
	reset_machine_state();
	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, ".123456789+00:00") != NULL);
	free(output);
}

/* ============================================================================
 * TEST COVERAGE: Sparse CPU identity across serializers
 * ============================================================================ */

static void seed_multi_cpu_inventory(void)
{
	struct cpu_inventory_seed cpus[3] = {
		{0, 1, 1, 0, 0, 0},
		{4, 1, 1, 0, 1, 0},
		{9, 1, 1, 0, 2, 0},
	};

	cpu_inventory_seed(cpus, 3);
}

static void make_multi_cpu_synthetic_record(struct interval_record *rec,
					    struct sys_snapshot *raw,
					    struct interval_stats *stats,
					    struct cpu_row *cpu_rows,
					    struct cpu_freq_info *freqs)
{
	static const int cpu_ids[3] = {0, 4, 9};
	static const int tracked_order[3] = {2, 0, 1};

	memset(rec, 0, sizeof(*rec));
	memset(raw, 0, sizeof(*raw));
	memset(stats, 0, sizeof(*stats));
	memset(cpu_rows, 0, 3 * sizeof(*cpu_rows));
	memset(freqs, 0, 3 * sizeof(*freqs));

	seed_multi_cpu_inventory();

	for (int i = 0; i < 3; i++) {
		freqs[i].cpu_id = cpu_ids[tracked_order[i]];
		freqs[i].cur_freq = (2000 + i * 100) * 1000;
		freqs[i].cur_freq_valid = 1;
		freqs[i].min_freq = 1700000;
		freqs[i].min_freq_valid = 1;
		freqs[i].max_freq = 2500000;
		freqs[i].max_freq_valid = 1;
		freqs[i].boost = 1;
		snprintf(freqs[i].governor, sizeof(freqs[i].governor),
			 "performance");

		cpu_rows[i].tracked_idx = tracked_order[i];
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
	rec->cpu_row_count = 3;
	rec->cpu_rows = cpu_rows;
	rec->summary.avg_mhz = stats->avg_mhz;
	rec->summary.busy_percent = stats->busy_percent;
	rec->summary.idle_percent = stats->avg_idle_percent;
	rec->summary.iowait_percent = stats->avg_iowait_percent;
}

static void test_sparse_cpu_identity_is_stable_across_serializers(void)
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
	parse_column_option("cpu,freq,idle", 1);
	reset_machine_state();
	make_multi_cpu_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	args.rec = &rec;
	assert(get_cpu_row_id(&rec, 0) == 9);
	assert(get_cpu_row_id(&rec, 1) == 0);
	assert(get_cpu_row_id(&rec, 2) == 4);
	assert(get_cpu_core(&rec, 0) == 2);
	assert(get_cpu_core(&rec, 1) == 0);
	assert(get_cpu_core(&rec, 2) == 1);

	output = capture_stdout(emit_csv_mixed_scope, &args);

	/* Header should contain Scope and CPU columns */
	assert(strstr(output, "Scope") != NULL);
	assert(strstr(output, "CPU") != NULL);
	assert(strstr(output, "schema_version") != NULL);

	/* Sparse Linux IDs remain paired with values from their record row. */
	assert(output_line_contains(output, ",CPU,9,", "2000.00"));
	assert(output_line_contains(output, ",CPU,0,", "2100.00"));
	assert(output_line_contains(output, ",CPU,4,", "2200.00"));

	/* Should contain a summary row */
	assert(strstr(output, ",SUM,") != NULL);
	assert_csv_rows_are_rectangular(output);

	free(output);

	reset_machine_state();
	output = capture_stdout(emit_json_record, &args);
	assert(output_line_contains(output, "\"cpu\": 9", "\"freq\": 2000.00"));
	assert(output_line_contains(output, "\"cpu\": 0", "\"freq\": 2100.00"));
	assert(output_line_contains(output, "\"cpu\": 4", "\"freq\": 2200.00"));
	free(output);

	output = capture_stdout(emit_text_record, &args);
	assert(output_line_contains(output, "9    ", "2000.00"));
	assert(output_line_contains(output, "0    ", "2100.00"));
	assert(output_line_contains(output, "4    ", "2200.00"));
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
	char *argv[] = {"armstat", "-f", "csv", "-s", "pkg_freq_mhz", NULL};

	parse_test_args(5, argv);
	reset_machine_state();
	make_synthetic_record(&rec, &raw, &stats, cpu_rows, freqs);
	rec.package_count = 1;
	rec.packages[0].package_id = 7;
	rec.packages[0].cpu_count = 1;
	rec.packages[0].avg_mhz = 2134.5;
	args.rec = &rec;

	output = capture_stdout(emit_csv_mixed_scope, &args);
	assert(strstr(output, "Package,freq") != NULL);
	assert(strstr(output, "9,1,1000000,") != NULL);
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
	char *argv[] = {"armstat", "-s", "all", "-f", "csv", NULL};

	parse_test_args(5, argv);
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

	/* Machine formats always use stable JSON field names. */
	assert(strstr(output, "timestamp_iso,Scope,freq,power") != NULL);

	/* Data row should have the schema_version value */
	assert(strstr(output, "9,") != NULL);
	assert(strstr(output, "timestamp_iso,Scope,") != NULL);

	/* Should contain a summary row */
	assert(strstr(output, ",SUM,") != NULL);

	free(output);

	set_section_summary_mode(0);
}

/* ============================================================================
 * TEST COVERAGE: interval_record materialization (owned values + independent LPI values)
 * ============================================================================ */

/*
 * Build a real interval_record from a fake raw snapshot + interval stats and
 * verify that all per-interval dynamic values are copied into the record's own
 * storage (cpu_rows, packages, numa_temps, summary) instead of dangling
 * pointers into the raw data. Also checks the build-time independent LPI values rule.
 */
static void test_interval_record_materializes_owned_values(void)
{
	struct sys_snapshot raw;
	struct interval_stats stats;
	struct cpu_freq_info freqs[1];
	struct idle_state states[2];
	struct idle_state *idle_arr[1];
	struct interval_record *rec;
	struct interval_record *pooled_rec;
	struct cpu_row *pooled_cpu_rows;

	reset_test_state();
	enable_cpu(1);
	enable_package(1);
	seed_single_cpu_inventory();
	cleanup_formatter_pool();
	assert(setup_formatter_pool(-1) < 0);
	assert(setup_formatter_pool(MAX_CPUS + 1) < 0);
	assert(setup_formatter_pool(1) == 0);

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

	/* Two usable idle states: state0 (20%), state1 (60%). */
	states[0].available = 1;
	states[0].disabled = 0;
	states[0].percentage = 20.0;
	states[0].usage_per_sec = 100.0;
	states[1].available = 1;
	states[1].disabled = 0;
	states[1].percentage = 60.0;
	states[1].usage_per_sec = 10.0;
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
	assert(!rec->cpu_rows_is_temp);
	pooled_rec = rec;
	pooled_cpu_rows = rec->cpu_rows;
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

	/* LPI values retain the sysfs deltas even when their sum differs from Idle%. */
	assert(rec->cpu_rows[0].idle_state_pct[0] == 20.0);
	assert(rec->cpu_rows[0].idle_state_pct[1] == 60.0);
	assert(rec->cpu_rows[0].idle_state_pct[0] +
	       rec->cpu_rows[0].idle_state_pct[1] == 80.0);
	assert(isnan(rec->cpu_rows[0].idle_state_pct[2]));
	assert(isnan(rec->cpu_rows[0].idle_state_pct[3]));
	/* Usage rates are owned per state; hidden states remain unavailable. */
	assert(rec->cpu_rows[0].idle_state_usage[0] == 100.0);
	assert(rec->cpu_rows[0].idle_state_usage[1] == 10.0);
	assert(isnan(rec->cpu_rows[0].idle_state_usage[2]));

	/* Summary states average the independent values and preserve missing data. */
	assert(rec->summary_idle_state_pct[0] == 20.0);
	assert(rec->summary_idle_state_pct[1] == 60.0);
	assert(isnan(rec->summary_idle_state_pct[2]));

	free_interval_record(rec);

	/* A rejected reconfiguration leaves the existing formatter pool usable. */
	assert(setup_formatter_pool(-1) < 0);

	/* Summary/package output must not materialize unused per-CPU rows. */
	enable_cpu(0);
	rec = build_interval_record(&raw, &stats, 2);
	assert(rec != NULL);
	assert(rec == pooled_rec);
	assert(rec->cpu_rows == pooled_cpu_rows);
	assert(rec->cpu_row_count == 0);
	assert(rec->package_count == 1);
	free_interval_record(rec);

	/* Summary-only mode also omits package rows, even if selected globally. */
	set_section_summary_mode(1);
	enable_cpu(1);
	rec = build_interval_record(&raw, &stats, 3);
	assert(rec != NULL);
	assert(rec->cpu_row_count == 0);
	assert(rec->package_count == 0);
	free_interval_record(rec);
	cleanup_formatter_pool();
}

static void test_lpi_records_preserve_independent_states(void)
{
	struct sys_snapshot raw = {0};
	struct interval_stats stats = {0};
	struct idle_state states[2][2] = {0};
	struct idle_state *idle_arr[2] = {states[0], states[1]};
	struct interval_record *rec;
	struct serializer_args args;
	char *output;

	reset_test_state();
	seed_two_cpu_inventory();
	enable_cpu(1);
	for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++) {
		show_idle_state[s] = s < 2;
		show_summary_idle_state[s] = s < 2;
	}
	for (int i = 0; i < 2; i++) {
		states[i][0].available = 1;
		states[i][1].available = 1;
		stats.per_cpu_idle[i] = NAN;
	}
	states[0][0].percentage = 10.0;
	states[1][0].percentage = 30.0;
	states[0][1].percentage = 50.0;
	states[1][1].percentage = 200.0;
	raw.effective_cpu_count = 2;
	raw.cpu_count = 2;
	raw.idle = idle_arr;
	raw.idle_state_count = 2;
	raw.interval_delta_us = 1000000;

	rec = build_interval_record(&raw, &stats, 1);
	assert(rec != NULL);
	assert(rec->cpu_rows[0].idle_state_pct[0] == 10.0);
	assert(rec->cpu_rows[1].idle_state_pct[1] == 200.0);
	assert(rec->summary_idle_state_pct[0] == 20.0);
	assert(rec->summary_idle_state_pct[1] == 125.0);
	args.rec = rec;

	/* Every serializer retains delayed settlement above 100%. */
	reset_machine_state();
	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"lpi1\": 125.00") != NULL);
	assert(output_line_contains(output, "\"cpu\": 4", "\"lpi1\": 200.00"));
	free(output);
	reset_machine_state();
	output = capture_stdout(emit_csv_mixed_scope, &args);
	assert(output_line_contains(output, ",SUM,", "125.00"));
	assert(output_line_contains(output, ",CPU,4,", "200.00"));
	assert_csv_rows_are_rectangular(output);
	free(output);
	output = capture_stdout(emit_text_record, &args);
	assert(strstr(output, "125.00") != NULL);
	assert(strstr(output, "200.00") != NULL);
	free(output);
	free_interval_record(rec);

	/* A missing state on one CPU invalidates only that summary state. */
	states[1][0].disabled = 1;
	rec = build_interval_record(&raw, &stats, 2);
	assert(rec != NULL);
	assert(rec->cpu_rows[0].idle_state_pct[0] == 10.0);
	assert(isnan(rec->cpu_rows[1].idle_state_pct[0]));
	assert(isnan(rec->summary_idle_state_pct[0]));
	assert(rec->summary_idle_state_pct[1] == 125.0);
	free_interval_record(rec);

	states[1][0].disabled = 0;
	states[1][1].percentage = NAN;
	rec = build_interval_record(&raw, &stats, 3);
	assert(rec != NULL);
	assert(rec->cpu_rows[0].idle_state_pct[1] == 50.0);
	assert(rec->summary_idle_state_pct[0] == 20.0);
	assert(isnan(rec->summary_idle_state_pct[1]));
	args.rec = rec;
	reset_machine_state();
	output = capture_stdout(emit_json_record, &args);
	assert(strstr(output, "\"lpi1\": null") != NULL);
	free(output);
	free_interval_record(rec);

	/* CPU and summary selections affect visibility only, independently. */
	states[1][1].percentage = 200.0;
	show_idle_state[0] = 0;
	show_summary_idle_state[1] = 0;
	rec = build_interval_record(&raw, &stats, 4);
	assert(rec != NULL);
	assert(isnan(rec->cpu_rows[0].idle_state_pct[0]));
	assert(rec->cpu_rows[0].idle_state_pct[1] == 50.0);
	assert(rec->cpu_rows[1].idle_state_pct[1] == 200.0);
	assert(rec->summary_idle_state_pct[0] == 20.0);
	assert(isnan(rec->summary_idle_state_pct[1]));
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
	test_default_busy_source_uses_procstat();
	test_automatic_ipc_success_preserves_selected_scopes();
	test_automatic_ipc_failure_hides_fields_and_clears_events();
	test_unselected_ipc_does_not_initialize_perf();
	test_explicit_pmu_failure_keeps_requested_columns();
	test_explicit_ipc_event_merge_and_hide_order();
	test_parse_summary_all_keeps_base_groups_only();
	test_parse_all_ipc_enables_default_pmu_pair();
	test_independent_metric_options_are_order_independent();
	test_repeated_show_options_form_a_union();
	test_repeated_show_does_not_restore_hidden_independent_requests();
	test_full_and_live_views_survive_repeated_show_options();
	test_parse_probe_and_busy_source();
	test_live_and_full_selection_have_distinct_data_surfaces();
	test_cpu_shortcut_retains_hidden_fields_and_independent_requests();
	test_mixed_scope_csv_serializer_uses_scoped_headers();
	test_summary_json_serializer_emits_schema_and_summary_only();
	test_unavailable_sensor_values_are_not_fake_zeroes();
	test_machine_string_and_boolean_semantics();
	test_default_machine_output_emits_summary_only();
	test_default_pmu_cpu_output_retains_package_topology();
	test_selected_json_package_has_unique_package_key();
	test_empty_json_selection_has_no_dangling_comma();
	test_empty_json_stream_closes_as_empty_array();
	test_text_serializer_emits_column_headers_and_values();
	test_text_header_interval_counts_complete_data_rows();
	test_compact_pmu_only_summary_headers_quiet_and_refresh();
	test_default_live_and_full_text_views();
	test_membw_opt_in_survives_selection_order_and_serialization();
	test_text_package_rows_align_large_sparse_ids();
	test_changed_columns_reprint_text_and_reject_csv();
	test_text_sample_blocks_have_visible_interval_boundaries();
	test_sparse_cpu_identity_is_stable_across_serializers();
	test_package_csv_serializer_emits_package_rows();
	test_all_scope_csv_serializer_emits_package_rows();
	test_summary_csv_serializer_emits_metadata_and_summary_fields();
	test_interval_record_materializes_owned_values();
	test_lpi_records_preserve_independent_states();
	test_hotplug_rebuild_refreshes_cpufreq_state();
	return 0;
}
