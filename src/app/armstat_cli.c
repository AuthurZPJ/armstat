/* SPDX-License-Identifier: GPL-2.0 */
/*
 * armstat_cli.c - Command-line parsing and column selection
 *
 * Parses CLI options into struct armstat_options and applies column
 * visibility selections.  Separated from armstat.c so the column-
 * selection tests can link against this layer without pulling in the
 * main event loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>

#include "armstat_cli.h"
#include "collector.h"
#include "aggregator.h"
#include "formatter.h"
#include "formatter_section.h"
#include "pmu.h"

#ifndef ARMSTAT_VERSION
#define ARMSTAT_VERSION "development"
#endif
#include "cpuidle.h"
#include "idle_backend.h"
#include "power.h"
#include "cpu_inventory.h"


/* Default options */
struct armstat_options default_options = {
	.interval = 1.0,
	.iterations = 0,
	.header_interval = TEXT_HEADER_AUTO,
	.output_file = NULL,
	.quiet = 0,
	.quiet_all = 0,
	.summary_mode = 0,
	.format = 0,  /* FORMAT_TEXT */
	.cpu_filter = NULL,
	.busy_source = NULL,
	.pmu_events = NULL,
	.ipc_requested = 0,
	.pmu_auto = 0,
	.no_perf = 0,
	.debug = 0,
	.list_counters = 0,
	.dump_raw = 0,
	.probe_only = 0,
	.command_argv = NULL,
};

/* ============================================================================
 * COMMAND LINE PARSING
 * ============================================================================ */

enum cli_option_value {
	OPT_PROBE = 256,
	OPT_QUIET_ALL,
};

static struct option long_options[] = {
	{"interval", required_argument, 0, 'i'},
	{"Dump", no_argument, 0, 'D'},
	{"num_iterations", required_argument, 0, 'n'},
	{"header_iterations", required_argument, 0, 'N'},
	{"cpu", required_argument, 0, 'c'},
	{"busy-source", required_argument, 0, 'B'},
	{"out", required_argument, 0, 'o'},
	{"quiet", no_argument, 0, 'q'},
	{"quiet-all", no_argument, 0, OPT_QUIET_ALL},
	{"show", required_argument, 0, 's'},
	{"hide", required_argument, 0, 'H'},
	{"debug", no_argument, 0, 'd'},
	{"version", no_argument, 0, 'v'},
	{"help", no_argument, 0, 'h'},
	{"format", required_argument, 0, 'f'},
	{"pmu-events", required_argument, 0, 'p'},
	{"list", no_argument, 0, 'l'},
	{"Summary", no_argument, 0, 'S'},
	{"Joules", no_argument, 0, 'J'},
	{"ipc", no_argument, 0, 'I'},
	{"all", no_argument, 0, 'a'},
	{"no-perf", no_argument, 0, 'P'},
	{"probe", no_argument, 0, OPT_PROBE},
	{0, 0, 0, 0}
};

static int ipc_selection_explicit;

static void print_version(void)
{
	printf("armstat version %s\n", ARMSTAT_VERSION);
	printf("ARM Server performance monitoring tool\n");
	printf("\n");
	printf("License GPLv2: GNU GPL version 2\n");
}

static void print_help(void)
{
	printf("Usage: armstat [options] [-- command [args...]]\n");
	printf("\n");
	printf("Default: SUM. -a: SUM + CPU live metrics. -s all: full base view.\n");
	printf("These views include IPC when PMU startup succeeds; -H ipc disables it.\n");
	printf("Package rows require -s pkg/package, a stable package field ID, or -s all.\n");
	printf("Text headers repeat to fit the terminal height by default.\n");
	printf("\n");
	printf("Options:\n");
	printf("  -i, --interval <sec>   Measurement interval (default: 1.0)\n");
	printf("  -n, --num_iterations <count> Samples to print (0 = unlimited)\n");
	printf("  -N, --header_iterations <count> Override automatic headers: every N samples (0 = startup/layout changes only)\n");
	printf("  -c, --cpu <list>       Real CPU IDs to monitor (e.g. 0,1,4-7)\n");
	printf("  -B, --busy-source <src> Busy/Idle source hint: auto, procstat, schedstat, task-clock\n");
	printf("  -o, --out <file>       Write statistics to file\n");
	printf("  -q, --quiet            Suppress the startup banner\n");
	printf("      --quiet-all        Also suppress text headers, timestamps, and markers\n");
	printf("  -D, --Dump             Dump one raw proc/sysfs snapshot in text and exit\n");
	printf("  -S, --Summary          Summary mode (SUM only)\n");
	printf("  -a, --all              Expand live metrics with CPU rows (use -s all for configuration and usage)\n");
	printf("  -f, --format <fmt>     Output format: text, json, ndjson, csv\n");
	printf("  -s, --show <items>     Show only selected groups or exact field names\n");
	printf("  -H, --hide <items>     Hide selected groups or exact field names\n");
	printf("  -p, --pmu-events <evts> Show raw PMU counters (add -I for IPC)\n");
	printf("  -I, --ipc              Enable cycles,instructions PMU + IPC columns\n");
	printf("  -J, --Joules           Show interval energy in Joules alongside power\n");
	printf("  -l, --list             List groups, exact fields, and PMU events\n");
	printf("  -P, --no-perf          Disable perf-backed PMU collection\n");
	printf("      --probe            Probe current platform capabilities and sources\n");
	printf("  -d, --debug            Enable debug output\n");
	printf("  -v, --version          Show version\n");
	printf("  -h, --help             Show help\n");
	printf("\nCommand mode measures one interval across the command's execution.\n");
	printf("Statistics go to stderr unless -o is used; the command retains its stdout.\n");
	printf("Use an explicit -- separator; -i, -n, -D, --probe, and -l cannot be combined with a command.\n");
	printf("--Dump cannot be combined with -i, -n, --probe, -l, non-text formats, or explicit IPC/PMU requests.\n");
	printf("\n");
	printf("Column groups:\n");
	printf("  cpu\n");
	printf("  pkg, package\n");
	printf("  core\n");
	printf("  numa, node\n");
	printf("  freq\n");
	printf("  freq-config\n");
	printf("  idle\n");
	printf("  idle-usage\n");
	printf("  power\n");
	printf("  temp\n");
	printf("  pmu\n");
	printf("  sysstat, irq\n");
	printf("  membw, mem\n");
	printf("  ipc\n");
	printf("  energy, joules\n");
	printf("  all\n");
	printf("\nGroups are case-insensitive and take precedence over stable IDs and aliases.\n");
}

static const char *field_scope_name(enum field_scope scope)
{
	switch (scope) {
	case FIELD_SCOPE_SYSTEM:
		return "summary";
	case FIELD_SCOPE_PACKAGE:
		return "package";
	case FIELD_SCOPE_CPU:
		return "cpu";
	default:
		return "unknown";
	}
}

static const char *field_type_name(enum field_type type)
{
	switch (type) {
	case FIELD_TYPE_INT:
		return "int";
	case FIELD_TYPE_LLONG:
		return "int64";
	case FIELD_TYPE_DOUBLE:
		return "number";
	case FIELD_TYPE_STRING:
		return "string";
	case FIELD_TYPE_BOOL:
		return "boolean";
	default:
		return "unknown";
	}
}

void list_counters(void)
{
	struct field_desc *fields = get_all_fields();
	int field_count = get_field_count();

	printf("Built-in column groups:\n");
	printf("  cpu\n");
	printf("  pkg, package\n");
	printf("  core\n");
	printf("  numa, node\n");
	printf("  freq\n");
	printf("  freq-config\n");
	printf("  idle\n");
	printf("  idle-usage\n");
	printf("  power\n");
	printf("  temp\n");
	printf("  pmu\n");
	printf("  sysstat, irq\n");
	printf("  membw, mem\n");
	printf("  ipc\n");
	printf("  energy, joules\n");
	printf("  all\n");
	printf("\n");
	printf("Group names are case-insensitive. cpu alone selects its usual live metrics.\n");
	printf("freq/idle select summary metrics; add cpu for per-CPU rows.\n");
	printf("\nExact fields (stable ID; scope; type; unit; text label; JSON key):\n");
	for (int i = 0; i < field_count; i++) {
		printf("  %-24s [%-7s] type=%-7s unit=%-18s text=%-12s json=%s\n",
		       fields[i].id ? fields[i].id : "",
		       field_scope_name(fields[i].scope),
		       field_type_name(fields[i].type),
		       fields[i].unit && fields[i].unit[0] ? fields[i].unit : "-",
		       fields[i].label ? fields[i].label : "",
		       fields[i].json_label ? fields[i].json_label : "");
	}
	printf("Group names take precedence; other stable IDs match one field before labels/JSON aliases.\n");
	printf("CPU-only aliases such as Min and lpi0_usage enable CPU rows; shared aliases do not.\n");
	printf("Use pkg/package for package rows; labels never implicitly enable package rows.\n");
	printf("Machine output uses JSON null / empty CSV cells for unavailable values.\n");
	printf("\n");
	printf("Built-in PMU events:\n");
	printf("Counts are per interval; mem-read/write count retired load/store instructions, not bytes.\n");
	list_builtin_pmu_events();
}

/*
 * Print the startup banner for text mode. Suppressed for machine-readable
 * formats and for explicit -q. Declared in armstat_cli.h.
 */
void print_interval_header(const struct armstat_options *opts, double interval)
{
	char interval_text[64];
	char *end;

	if (opts->format != FORMAT_TEXT)
		return;
	if (opts->quiet)
		return;

	snprintf(interval_text, sizeof(interval_text), "%.6f", interval);
	end = interval_text + strlen(interval_text);
	while (end > interval_text && end[-1] == '0')
		*--end = '\0';
	if (end > interval_text && end[-1] == '.')
		*--end = '\0';

	printf("armstat - ARM Server Performance Monitor\n");
	printf("Sampling interval: %s second(s)\n", interval_text);
	printf("\n");
}

struct column_alias_group {
	const char *const *aliases;
	int alias_count;
	unsigned int field_group_mask;
	void (*set_visible)(int enable);
};

int parse_column_option(const char *arg, int enable);

/* Track unmatched tokens for whitelist validation */
static int unknown_column_count;

static void set_idle_columns(int enable)
{
	enable_idle(enable);
	enable_iowait(enable);
	if (enable)
		enable_cpuidle(1);
}

static void set_idle_usage_columns(int enable)
{
	enable_idle_usage(enable);
	if (enable)
		enable_cpuidle(1);
}

static void set_core_columns(int enable)
{
	enable_core(enable);
	if (enable)
		enable_cpu(1);
}

static void set_numa_columns(int enable)
{
	enable_numa(enable);
	if (enable)
		enable_cpu(1);
}

static int option_matches_alias_group(const char *arg,
				      const struct column_alias_group *group)
{
	for (int i = 0; i < group->alias_count; i++) {
		if (strcasecmp(arg, group->aliases[i]) == 0)
			return 1;
	}

	return 0;
}

static void set_group_field_overrides(unsigned int group_mask, int enable, int whitelist)
{
	struct field_desc *fields = get_all_fields();
	int field_count = get_field_count();

	for (int i = 0; i < field_count; i++) {
		if (group_mask != FIELD_GROUP_NONE &&
		    (fields[i].group_mask & group_mask) != 0)
			set_field_override_by_index(i, enable, whitelist);
	}
}

static int field_matches_token(const struct field_desc *field, const char *token)
{
	if (!field || !token || !*token)
		return 0;

	if (field->id && strcmp(field->id, token) == 0)
		return 1;
	if (field->label && strcmp(field->label, token) == 0)
		return 1;
	if (field->json_label && strcmp(field->json_label, token) == 0)
		return 1;

	return 0;
}

void set_all_columns_enabled(int enable)
{
	enable_cpu(enable);
	enable_freq(enable);
	enable_freq_config(enable);
	set_idle_columns(enable);
	set_idle_usage_columns(enable);
	enable_power(enable);
	enable_temp(enable);
	enable_sysstat(enable);
	enable_numa(enable);
	enable_package(enable);
	enable_core(enable);
	enable_energy(enable);
	enable_ipc(enable);

	/* Broad views include IPC; raw PMU and rare bandwidth sources are opt-in. */
	if (!enable) {
		enable_pmu(0);
		enable_membw(0);
	}
}

static void set_live_columns_enabled(void)
{
	set_all_columns_enabled(1);
	enable_freq_config(0);
	enable_idle_usage(0);
	set_section_package_output(0);
}

static void select_view_fields(int live_only)
{
	struct field_desc *fields = get_all_fields();
	int field_count = get_field_count();

	/* Keep broad views explicit so later -s additions remain a union. */
	for (int i = 0; i < field_count; i++) {
		if ((fields[i].group_mask & FIELD_GROUP_IPC) && !is_ipc_enabled())
			continue;
		if ((fields[i].group_mask & FIELD_GROUP_MEMBW) && !is_membw_enabled())
			continue;
		if (live_only && (fields[i].group_mask &
		    (FIELD_GROUP_FREQ_CONFIG | FIELD_GROUP_IDLE_USAGE)))
			continue;
		set_field_override_by_index(i, 1, 1);
	}
}

static int column_list_is_cpu_only(const char *arg)
{
	const char *start = arg;

	while (start && *start) {
		const char *end = strchr(start, ',');
		const char *trim_end = end ? end : start + strlen(start);

		while (*start == ' ' || *start == '\t')
			start++;
		while (trim_end > start &&
		       (trim_end[-1] == ' ' || trim_end[-1] == '\t'))
			trim_end--;
		if (trim_end - start != 3 || strncasecmp(start, "cpu", 3))
			return 0;
		start = end ? end + 1 : NULL;
	}
	return 1;
}

static int parse_positive_double_arg(const char *option, const char *arg,
				     double *value)
{
	const double min_interval = 0.000001;
	char *end = NULL;
	double parsed;

	errno = 0;
	parsed = strtod(arg, &end);
	if (errno || end == arg || (end && *end) ||
	    !isfinite(parsed) || parsed < min_interval || parsed > 31536000.0) {
		fprintf(stderr,
			"Error: %s requires a value from %.6f to 31536000 seconds: %s\n",
			option, min_interval, arg ? arg : "");
		return -1;
	}

	*value = parsed;
	return 0;
}

static int parse_non_negative_int_arg(const char *option, const char *arg,
				      int *value)
{
	char *end = NULL;
	long parsed;

	errno = 0;
	parsed = strtol(arg, &end, 10);
	if (errno || end == arg || (end && *end) ||
	    parsed < 0 || parsed > INT_MAX) {
		fprintf(stderr, "Error: %s requires a non-negative integer: %s\n",
			option, arg ? arg : "");
		return -1;
	}

	*value = (int)parsed;
	return 0;
}

static int apply_busy_source_option(struct armstat_options *opts, const char *arg)
{
	opts->busy_source = (char *)arg;

	if (strcmp(arg, "procstat") == 0)
		set_busy_source_mode(BUSY_SOURCE_PROCSTAT);
	else if (strcmp(arg, "schedstat") == 0)
		set_busy_source_mode(BUSY_SOURCE_SCHEDSTAT);
	else if (strcmp(arg, "task-clock") == 0)
		set_busy_source_mode(BUSY_SOURCE_TASK_CLOCK);
	else if (strcmp(arg, "auto") == 0)
		set_busy_source_mode(BUSY_SOURCE_AUTO);
	else {
		fprintf(stderr, "Error: unknown busy source '%s'\n", arg);
		return -1;
	}

	return 0;
}

static int apply_show_option(const char *arg, int reset_selection)
{
	/* The first -s starts a whitelist; repeated -s options extend it. */
	if (!arg || !*arg) {
		fprintf(stderr, "Error: --show requires at least one column group or field name\n");
		return -1;
	}
	if (reset_selection) {
		clear_columns();
		ipc_selection_explicit = 0;
	}
	unknown_column_count = 0;
	if (parse_column_option(arg, 1) < 0)
		return -1;
	set_section_default_summary_output(1);
	return 0;
}

static int apply_hide_option(const char *arg)
{
	/* Hide blacklist: disable specified columns */
	if (!arg || !*arg) {
		fprintf(stderr, "Error: --hide requires at least one column group or field name\n");
		return -1;
	}
	unknown_column_count = 0;
	if (parse_column_option(arg, 0) < 0)
		return -1;
	return 0;
}

static void apply_ipc_option(struct armstat_options *opts)
{
	/* IPC - enable PMU and show IPC.
	 * pmu_events is not set here; apply_default_pmu_events() merges
	 * cycles,instructions into whatever -p already provided. */
	opts->ipc_requested = 1;
	enable_pmu(1);
	enable_ipc(1);
}

static int apply_format_option(struct armstat_options *opts, const char *arg)
{
	if (strcmp(arg, "json") == 0)
		opts->format = FORMAT_JSON;
	else if (strcmp(arg, "ndjson") == 0)
		opts->format = FORMAT_NDJSON;
	else if (strcmp(arg, "csv") == 0)
		opts->format = FORMAT_CSV;
	else if (strcmp(arg, "text") == 0)
		opts->format = FORMAT_TEXT;
	else {
		fprintf(stderr, "Error: unknown output format '%s'\n", arg);
		return -1;
	}

	return 0;
}

/*
 * Parse column visibility string (for -s/-H options)
 */
static int column_list_has_empty_token(const char *list)
{
	int need_value = 1;

	for (const char *p = list; p && *p; p++) {
		if (*p == ',') {
			if (need_value)
				return 1;
			need_value = 1;
			continue;
		}
		if (*p != ' ' && *p != '\t')
			need_value = 0;
	}

	return need_value;
}

int parse_column_option(const char *arg, int enable)
{
	static const char *const cpu_aliases[] = {"cpu"};
	static const char *const package_aliases[] = {"pkg", "package"};
	static const char *const core_aliases[] = {"core"};
	static const char *const numa_aliases[] = {"numa", "node"};
	static const char *const freq_aliases[] = {"freq"};
	static const char *const freq_config_aliases[] = {"freq-config"};
	static const char *const idle_aliases[] = {"idle"};
	static const char *const idle_usage_aliases[] = {"idle-usage"};
	static const char *const power_aliases[] = {"power"};
	static const char *const temp_aliases[] = {"temp"};
	static const char *const pmu_aliases[] = {"pmu"};
	static const char *const sysstat_aliases[] = {"sysstat", "irq"};
	static const char *const membw_aliases[] = {"membw", "mem"};
	static const char *const ipc_aliases[] = {"ipc"};
	static const char *const energy_aliases[] = {"energy", "joules"};
	static const struct column_alias_group groups[] = {
		{cpu_aliases, ARRAY_SIZE(cpu_aliases),
		 FIELD_GROUP_NONE, enable_cpu},
		{package_aliases, ARRAY_SIZE(package_aliases),
		 FIELD_GROUP_PACKAGE, enable_package},
		{core_aliases, ARRAY_SIZE(core_aliases),
		 FIELD_GROUP_CORE, set_core_columns},
		{numa_aliases, ARRAY_SIZE(numa_aliases),
		 FIELD_GROUP_NUMA, set_numa_columns},
		{freq_aliases, ARRAY_SIZE(freq_aliases),
		 FIELD_GROUP_FREQ, enable_freq},
		{freq_config_aliases, ARRAY_SIZE(freq_config_aliases),
		 FIELD_GROUP_FREQ_CONFIG, enable_freq_config},
		{idle_aliases, ARRAY_SIZE(idle_aliases),
		 FIELD_GROUP_IDLE, set_idle_columns},
		{idle_usage_aliases, ARRAY_SIZE(idle_usage_aliases),
		 FIELD_GROUP_IDLE_USAGE, set_idle_usage_columns},
		{power_aliases, ARRAY_SIZE(power_aliases),
		 FIELD_GROUP_POWER, enable_power},
		{temp_aliases, ARRAY_SIZE(temp_aliases),
		 FIELD_GROUP_TEMP, enable_temp},
		{pmu_aliases, ARRAY_SIZE(pmu_aliases),
		 FIELD_GROUP_NONE, enable_pmu},
		{sysstat_aliases, ARRAY_SIZE(sysstat_aliases),
		 FIELD_GROUP_SYSSTAT, enable_sysstat},
		{membw_aliases, ARRAY_SIZE(membw_aliases),
		 FIELD_GROUP_MEMBW, enable_membw},
		{ipc_aliases, ARRAY_SIZE(ipc_aliases),
		 FIELD_GROUP_IPC, enable_ipc},
		{energy_aliases, ARRAY_SIZE(energy_aliases),
		 FIELD_GROUP_ENERGY, enable_energy},
	};
	char *arg_copy, *token, *saveptr = NULL;
	struct field_desc *fields = get_all_fields();
	int field_count = get_field_count();

	if (!arg || !*arg || column_list_has_empty_token(arg)) {
		fprintf(stderr, "Error: column list contains an empty token\n");
		return -1;
	}

	arg_copy = strdup(arg);
	if (!arg_copy) {
		fprintf(stderr, "Error: out of memory while parsing column list\n");
		return -1;
	}

	for (token = strtok_r(arg_copy, ",", &saveptr);
	     token;
	     token = strtok_r(NULL, ",", &saveptr)) {
		while (*token == ' ' || *token == '\t')
			token++;

		char *end = token + strlen(token);
		while (end > token && (end[-1] == ' ' || end[-1] == '\t')) {
			end--;
			*end = '\0';
		}

		if (*token == '\0')
			continue;

		if (strcasecmp(token, "all") == 0) {
			set_all_columns_enabled(enable);
			if (enable)
				select_view_fields(0);
			else
				clear_field_overrides();
			continue;
		}

		if (strcasecmp(token, "idle") == 0)
			clear_idle_state_overrides();

		int matched = 0;
		for (int i = 0; i < ARRAY_SIZE(groups); i++) {
			if (option_matches_alias_group(token, &groups[i])) {
				groups[i].set_visible(enable);
				if (enable && (groups[i].field_group_mask & FIELD_GROUP_IPC))
					ipc_selection_explicit = 1;
				if (enable && (groups[i].field_group_mask &
				    (FIELD_GROUP_FREQ_CONFIG | FIELD_GROUP_IDLE_USAGE)))
					enable_cpu(1);
				set_group_field_overrides(groups[i].field_group_mask,
							 enable, enable);
				matched = 1;
				break;
			}
		}

		if (matched)
			continue;

		int exact_id = 0;
		unsigned int alias_scopes = 0;
		for (int i = 0; i < field_count; i++) {
			if (field_matches_token(&fields[i], token))
				alias_scopes |= 1U << fields[i].scope;
			if (fields[i].id && strcmp(fields[i].id, token) == 0) {
				exact_id = 1;
			}
		}

		for (int i = 0; i < field_count; i++) {
			if (exact_id ? strcmp(fields[i].id, token) != 0 :
			    !field_matches_token(&fields[i], token))
				continue;

			/*
			 * Exact field tokens refine a group, they do not redefine the
			 * whole group.
			 *
			 * - Show/whitelist mode still needs the coarse group flag so the
			 *   data source gets sampled.
			 * - Hide/blacklist mode must not clear the group flag, otherwise
			 *   hiding a single field such as "uncore_freq" would also hide
			 *   its sibling current-frequency fields.
			 */
			if (enable)
				*fields[i].enabled_ptr = 1;
			if (enable && (fields[i].group_mask & FIELD_GROUP_IPC))
				ipc_selection_explicit = 1;
			if (enable && exact_id && fields[i].scope == FIELD_SCOPE_PACKAGE)
				enable_package(1);
			if (enable && fields[i].scope == FIELD_SCOPE_CPU &&
			    (exact_id || alias_scopes == (1U << FIELD_SCOPE_CPU)))
				enable_cpu(1);
			set_field_override_by_index(i, enable, enable);
			matched = 1;

			if (field_series_is_idle_state(fields[i].series) &&
			    fields[i].series_index >= 0) {
				if (enable)
					enable_cpuidle(1);
			}
		}

		if (!matched) {
			fprintf(stderr, "Error: unknown column group/field '%s'\n", token);
			unknown_column_count++;
		}
	}

	free(arg_copy);
	return unknown_column_count > 0 ? -1 : 0;
}

/*
 * Parse command line arguments
 */
/*
 * Returns 0 to continue, >0 for a successful early exit, and <0 for a
 * command-line error.
 */
int parse_args(int argc, char *argv[], struct armstat_options *opts)
{
	int opt;
	int option_position;
	int command_separator = 0;
	int interval_option_seen = 0;
	int iterations_option_seen = 0;
	int explicit_perf_requested = opts->ipc_requested || opts->pmu_events;
	int energy_requested = 0;
	int show_option_seen = 0;
	int view_option_seen = 0;
	int cpu_only_selection = 1;

	ipc_selection_explicit = 0;
	set_cpu_inventory_filter(NULL);
	set_section_summary_mode(0);
	set_section_default_summary_output(1);
	set_text_quiet(opts->quiet_all);

	for (;;) {
		option_position = optind > 0 ? optind : 1;
		opt = getopt_long(argc, argv, "+i:n:N:c:B:o:qDSf:s:H:df:p:lJIPv?ah",
				  long_options, NULL);
		if (opt == -1) {
			command_separator = option_position < argc &&
				!strcmp(argv[option_position], "--");
			break;
		}
		switch (opt) {
		case 'i':
			if (parse_positive_double_arg("--interval", optarg,
						      &opts->interval) < 0)
				return -1;
			interval_option_seen = 1;
			break;
		case 'n':
			if (parse_non_negative_int_arg("--num_iterations", optarg,
						       &opts->iterations) < 0)
				return -1;
			iterations_option_seen = 1;
			break;
		case 'N':
			if (parse_non_negative_int_arg("--header_iterations", optarg,
						       &opts->header_interval) < 0)
				return -1;
			set_text_header_interval(opts->header_interval);
			break;
		case 'q':
			opts->quiet = 1;
			break;
		case OPT_QUIET_ALL:
			opts->quiet = 1;
			opts->quiet_all = 1;
			set_text_quiet(1);
			break;
		case 'D':
			opts->dump_raw = 1;
			break;
		case 'S':
			opts->summary_mode = 1;
			set_section_summary_mode(1);
			break;
		case 'a':
			cpu_only_selection = 0;
			view_option_seen = 1;
			ipc_selection_explicit = 0;
			set_live_columns_enabled();
			clear_field_overrides();
			select_view_fields(1);
			set_section_default_summary_output(1);
			break;
		case 'c':
			opts->cpu_filter = optarg;
			if (set_cpu_inventory_filter(optarg) < 0)
				return -1;
			break;
		case 'B':
			if (apply_busy_source_option(opts, optarg) < 0)
				return -1;
			break;
		case 'o':
			opts->output_file = optarg;
			break;
		case 's':
			if (!show_option_seen)
				cpu_only_selection = column_list_is_cpu_only(optarg);
			else
				cpu_only_selection &= column_list_is_cpu_only(optarg);
			if (apply_show_option(optarg, !show_option_seen) < 0)
				return -1;
			/* Only the first whitelist clears independent metric requests. */
			if (!show_option_seen) {
				if (opts->pmu_events)
					enable_pmu(1);
				if (opts->ipc_requested) {
					enable_pmu(1);
					enable_ipc(1);
					set_group_field_overrides(FIELD_GROUP_IPC, 1, 1);
				}
				if (energy_requested) {
					enable_energy(1);
					set_group_field_overrides(FIELD_GROUP_ENERGY, 1, 1);
				}
			}
			show_option_seen = 1;
			view_option_seen = 1;
			explicit_perf_requested |= ipc_selection_explicit || is_pmu_enabled();
			break;
		case 'H':
			if (apply_hide_option(optarg) < 0)
				return -1;
			break;
		case 'd':
			opts->debug = 1;
			break;
		case 'l':
			opts->list_counters = 1;
			break;
		case 'J':
			/* Energy in Joules */
			energy_requested = 1;
			reset_energy();
			enable_energy(1);
			if (view_option_seen)
				set_group_field_overrides(FIELD_GROUP_ENERGY, 1, 1);
			break;
		case 'I':
			explicit_perf_requested = 1;
			apply_ipc_option(opts);
			set_group_field_overrides(FIELD_GROUP_IPC, 1, view_option_seen);
			break;
		case 'P':
			opts->no_perf = 1;
			break;
		case OPT_PROBE:
			opts->probe_only = 1;
			break;
		case 'f':
			if (apply_format_option(opts, optarg) < 0)
				return -1;
			break;
		case 'p':
			if (validate_pmu_event_list(optarg) < 0)
				return -1;
			opts->pmu_events = optarg;
			explicit_perf_requested = 1;
			enable_pmu(1);
			break;
		case 'v':
			print_version();
			return 1;  /* Exit without error */
		case 'h':
			print_help();
			return 1;  /* Exit without error */
		case '?':
			print_help();
			if (optind > 0 && strcmp(argv[optind - 1], "-?") == 0)
				return 1;  /* Exit without error */
			return -1;  /* Exit with error */
		default:
			print_help();
			return -1;  /* Exit with error */
		}
	}

	if (command_separator) {
		if (optind >= argc || !argv[optind][0]) {
			fprintf(stderr, "Error: -- requires a command\n");
			return -1;
		}
		if (interval_option_seen || iterations_option_seen || opts->dump_raw ||
		    opts->probe_only || opts->list_counters) {
			fprintf(stderr, "Error: command mode cannot be combined with -i, -n, -D, --probe, or -l\n");
			return -1;
		}
		opts->command_argv = argv + optind;
	} else if (optind < argc) {
		fprintf(stderr, "Error: unexpected argument: %s\n", argv[optind]);
		fprintf(stderr, "  Use -- before a command and its arguments\n");
		return -1;
	}

	if (opts->no_perf && explicit_perf_requested) {
		fprintf(stderr, "Error: --no-perf cannot be combined with explicit IPC or PMU requests\n");
		return -1;
	}
	if (opts->dump_raw && (interval_option_seen || iterations_option_seen ||
	    opts->probe_only || opts->list_counters || opts->format != FORMAT_TEXT ||
	    explicit_perf_requested)) {
		fprintf(stderr, "Error: --Dump requires text and cannot be combined with -i, -n, --probe, -l, or explicit IPC/PMU requests\n");
		return -1;
	}

	if (show_option_seen && cpu_only_selection && show_cpu) {
		/* A row-only CPU request inherits useful live metrics, not just IDs. */
		enable_freq(1);
		enable_idle(1);
		enable_iowait(1);
		enable_temp(1);
		enable_ipc(1);
		select_default_cpu_fields();
		set_section_default_summary_output(energy_requested ||
					   opts->ipc_requested || opts->pmu_events);
	}

	return 0;  /* Continue execution */
}

static int selected_ipc_fields(void)
{
	struct field_desc *fields[64];
	int count;

	for (int scope = FIELD_SCOPE_SYSTEM; scope <= FIELD_SCOPE_CPU; scope++) {
		if (scope == FIELD_SCOPE_PACKAGE ||
		    (scope == FIELD_SCOPE_SYSTEM && !section_is_summary_mode() &&
		     !section_emit_default_summary()) ||
		    (scope == FIELD_SCOPE_CPU &&
		     (section_is_summary_mode() || !section_emit_cpu())))
			continue;
		get_enabled_fields(scope, fields, &count);
		for (int i = 0; i < count; i++) {
			if (fields[i]->group_mask & FIELD_GROUP_IPC)
				return 1;
		}
	}
	return 0;
}

int apply_default_pmu_events(struct armstat_options *opts)
{
	int explicit_ipc = opts->ipc_requested || ipc_selection_explicit;
	int want_ipc;

	if (opts->no_perf || opts->dump_raw) {
		opts->pmu_auto = 0;
		opts->pmu_events = NULL;
		enable_pmu(0);
		close_pmu_events();
		set_ipc_field_availability(0);
		return 0;
	}

	/* A custom raw event list must not silently acquire additional events. */
	if (opts->pmu_events && !explicit_ipc)
		set_ipc_field_availability(0);
	want_ipc = selected_ipc_fields();
	opts->pmu_auto = want_ipc && !explicit_ipc && !is_pmu_enabled();
	if (!opts->pmu_events && (is_pmu_enabled() || want_ipc))
		opts->pmu_events = "cycles,instructions";

	/*
	 * If IPC was explicitly selected, ensure its baseline pair is in the event
	 * list so IPC can actually be computed.  Merge them in front of any
	 * existing -p events without duplicating.
	 */
	if ((opts->ipc_requested || (ipc_selection_explicit && want_ipc)) &&
	    opts->pmu_events) {
		static char merged[512];
		int has_cycles = 0, has_instructions = 0;
		int written = 0;
		const char *p = opts->pmu_events;

		while (*p) {
			const char *start = p;
			const char *next = strchr(p, ',');
			const char *end = next ? next : p + strlen(p);
			while (start < end && (*start == ' ' || *start == '\t'))
				start++;
			while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
				end--;
			if ((end - start) == 6 && strncmp(start, "cycles", 6) == 0)
				has_cycles = 1;
			if ((end - start) == 12 && strncmp(start, "instructions", 12) == 0)
				has_instructions = 1;
			p = next ? next + 1 : p + strlen(p);
		}

		if (!has_cycles || !has_instructions) {
			if (!has_cycles && !has_instructions)
				written = snprintf(merged, sizeof(merged),
						   "cycles,instructions,%s", opts->pmu_events);
			else if (!has_cycles)
				written = snprintf(merged, sizeof(merged),
						   "cycles,%s", opts->pmu_events);
			else
				written = snprintf(merged, sizeof(merged),
						   "instructions,%s", opts->pmu_events);
			if (written < 0 || (size_t)written >= sizeof(merged)) {
				fprintf(stderr, "Error: combined PMU event list is too long\n");
				return -1;
			}
			opts->pmu_events = merged;
		}
	}

	if (opts->pmu_events && validate_pmu_event_list(opts->pmu_events) < 0)
		return -1;

	return 0;
}

int initialize_selected_pmu(const struct armstat_options *opts,
			    int (*init_events)(const char *events))
{
	int result;

	if (opts->no_perf || opts->dump_raw) {
		close_pmu_events();
		set_ipc_field_availability(0);
		return 0;
	}

	set_pmu_diagnostics(!opts->pmu_auto);
	if (!opts->pmu_events || (!is_pmu_enabled() && !selected_ipc_fields()))
		return 0;
	if (opts->debug)
		fprintf(stderr, "Initializing PMU: %s\n", opts->pmu_events);
	result = init_events(opts->pmu_events);
	if (result < 0) {
		if (opts->pmu_auto) {
			/* Do not retry an unavailable automatic source during hotplug. */
			close_pmu_events();
			set_ipc_field_availability(0);
			if (opts->debug)
				fprintf(stderr, "Automatic IPC unavailable; skipping IPC columns\n");
		} else {
			fprintf(stderr, "Warning: PMU init failed; PMU columns will be shown as unavailable\n");
			fprintf(stderr, "  Events: %s\n", opts->pmu_events);
			fprintf(stderr, "  Note: check perf permissions, kernel support, or event availability\n");
		}
	} else if (opts->debug) {
		fprintf(stderr, "PMU initialized successfully\n");
	}
	return result;
}
