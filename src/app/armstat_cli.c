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
	.header_interval = 0,
	.output_file = NULL,
	.quiet = 0,
	.summary_mode = 0,
	.format = 0,  /* FORMAT_TEXT */
	.cpu_filter = NULL,
	.busy_source = NULL,
	.pmu_events = NULL,
	.ipc_requested = 0,
	.debug = 0,
	.list_counters = 0,
	.dump_once = 0,
	.probe_only = 0,
};

/* ============================================================================
 * COMMAND LINE PARSING
 * ============================================================================ */

static struct option long_options[] = {
	{"interval", required_argument, 0, 'i'},
	{"dump", no_argument, 0, 'D'},
	{"num-iterations", required_argument, 0, 'n'},
	{"header-iterations", required_argument, 0, 'N'},
	{"cpu", required_argument, 0, 'c'},
	{"busy-source", required_argument, 0, 'B'},
	{"output", required_argument, 0, 'o'},
	{"export", required_argument, 0, 'O'},
	{"quiet", no_argument, 0, 'q'},
	{"show", required_argument, 0, 's'},
	{"hide", required_argument, 0, 'H'},
	{"debug", no_argument, 0, 'd'},
	{"version", no_argument, 0, 'v'},
	{"help", no_argument, 0, 'h'},
	{"format", required_argument, 0, 'f'},
	{"pmu-events", required_argument, 0, 'p'},
	{"list", no_argument, 0, 'l'},
	{"summary", no_argument, 0, 'S'},
	{"joules", no_argument, 0, 'J'},
	{"ipc", no_argument, 0, 'I'},
	{"all", no_argument, 0, 'a'},
	{"probe", no_argument, 0, 'P'},
	{0, 0, 0, 0}
};

static void print_version(void)
{
	printf("armstat version %s\n", ARMSTAT_VERSION);
	printf("ARM Server performance monitoring tool\n");
	printf("\n");
	printf("License GPLv2: GNU GPL version 2\n");
}

static void print_help(void)
{
	printf("Usage: armstat [options]\n");
	printf("\n");
	printf("Options:\n");
	printf("  -i, --interval <sec>   Measurement interval (default: 1.0)\n");
	printf("  -n, --num-iterations <count> Samples to print (0 = unlimited)\n");
	printf("  -N, --header-iterations <count> Reprint text header every N samples (0 = first only)\n");
	printf("  -c, --cpu <list>       Real CPU IDs to monitor (e.g. 0,1,4-7)\n");
	printf("  -B, --busy-source <src> Busy/Idle source hint: auto, procstat, schedstat, task-clock\n");
	printf("  -o, --output <file>    Write output to file\n");
	printf("  -O, --export <file>    Alias for --output\n");
	printf("  -q, --quiet            Suppress text banner and headers\n");
	printf("  -D, --dump             Collect one complete interval and exit\n");
	printf("  -S, --summary          Summary mode (SUM only)\n");
	printf("  -a, --all              Enable all supported base column groups (use -I for IPC, -p for PMU)\n");
	printf("  -f, --format <fmt>     Output format: text, json, csv\n");
	printf("  -s, --show <items>     Show only selected groups or exact field names\n");
	printf("  -H, --hide <items>     Hide selected groups or exact field names\n");
	printf("  -p, --pmu-events <evts> Enable PMU events\n");
	printf("  -I, --ipc              Enable cycles,instructions PMU + IPC columns\n");
	printf("  -J, --joules           Show interval energy in Joules\n");
	printf("  -l, --list             List groups, exact fields, and PMU events\n");
	printf("  -P, --probe            Probe current platform capabilities and sources\n");
	printf("  -d, --debug            Enable debug output\n");
	printf("  -v, --version          Show version\n");
	printf("  -h, --help             Show help\n");
	printf("\n");
	printf("Column groups:\n");
	printf("  cpu\n");
	printf("  pkg, package\n");
	printf("  core\n");
	printf("  numa, node\n");
	printf("  freq\n");
	printf("  idle\n");
	printf("  power\n");
	printf("  temp\n");
	printf("  pmu\n");
	printf("  sysstat, irq\n");
	printf("  membw, mem\n");
	printf("  ipc\n");
	printf("  energy, joules\n");
	printf("  all\n");
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
	printf("  idle\n");
	printf("  power\n");
	printf("  temp\n");
	printf("  pmu\n");
	printf("  sysstat, irq\n");
	printf("  membw, mem\n");
	printf("  ipc\n");
	printf("  energy, joules\n");
	printf("  all\n");
	printf("\n");
	printf("Use lowercase names with -s/-H (e.g. -s cpu,freq,power).\n");
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
	printf("Stable IDs are unambiguous exact -s/-H tokens; labels and JSON keys also work when they do not collide with group aliases.\n");
	printf("Machine output uses JSON null / empty CSV cells for unavailable values.\n");
	printf("\n");
	printf("Built-in PMU events:\n");
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

	if (opts->format == FORMAT_JSON || opts->format == FORMAT_CSV)
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
	const char *group_key;
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
	enable_cpuidle(enable);
}

static int option_matches_alias_group(const char *arg,
				      const struct column_alias_group *group)
{
	for (int i = 0; i < group->alias_count; i++) {
		if (strcmp(arg, group->aliases[i]) == 0)
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
	set_idle_columns(enable);
	enable_power(enable);
	enable_temp(enable);
	enable_sysstat(enable);
	enable_membw(enable);
	enable_numa(enable);
	enable_package(enable);
	enable_core(enable);
	enable_energy(enable);

	/*
	 * PMU/IPC are intentionally excluded from the implicit "all columns"
	 * toggle. Unlike the other groups they are not just display choices:
	 * enabling them starts perf-based runtime collection, changes failure
	 * modes, and may require additional privileges. Keep them explicit via
	 * -p or -I so "-a" remains a safe "show the normal data surface" option.
	 *
	 * When disabling all groups (for example via -H all), still turn PMU/IPC
	 * off so the blacklist semantics stay intuitive.
	 */
	if (!enable) {
		enable_pmu(0);
		enable_ipc(0);
	}
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
	if (reset_selection)
		clear_columns();
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
	static const char *const idle_aliases[] = {"idle"};
	static const char *const power_aliases[] = {"power"};
	static const char *const temp_aliases[] = {"temp"};
	static const char *const pmu_aliases[] = {"pmu"};
	static const char *const sysstat_aliases[] = {"sysstat", "irq"};
	static const char *const membw_aliases[] = {"membw", "mem"};
	static const char *const ipc_aliases[] = {"ipc"};
	static const char *const energy_aliases[] = {"energy", "joules"};
	static const struct column_alias_group groups[] = {
		{"cpu", cpu_aliases, ARRAY_SIZE(cpu_aliases), FIELD_GROUP_NONE, enable_cpu},
		{"package", package_aliases, ARRAY_SIZE(package_aliases), FIELD_GROUP_PACKAGE, enable_package},
		{"core", core_aliases, ARRAY_SIZE(core_aliases), FIELD_GROUP_CORE, enable_core},
		{"numa", numa_aliases, ARRAY_SIZE(numa_aliases), FIELD_GROUP_NUMA, enable_numa},
		{"freq", freq_aliases, ARRAY_SIZE(freq_aliases), FIELD_GROUP_FREQ, enable_freq},
		{"idle", idle_aliases, ARRAY_SIZE(idle_aliases), FIELD_GROUP_IDLE, set_idle_columns},
		{"power", power_aliases, ARRAY_SIZE(power_aliases), FIELD_GROUP_POWER, enable_power},
		{"temp", temp_aliases, ARRAY_SIZE(temp_aliases), FIELD_GROUP_TEMP, enable_temp},
		{"pmu", pmu_aliases, ARRAY_SIZE(pmu_aliases), FIELD_GROUP_NONE, enable_pmu},
		{"sysstat", sysstat_aliases, ARRAY_SIZE(sysstat_aliases), FIELD_GROUP_SYSSTAT, enable_sysstat},
		{"membw", membw_aliases, ARRAY_SIZE(membw_aliases), FIELD_GROUP_MEMBW, enable_membw},
		{"ipc", ipc_aliases, ARRAY_SIZE(ipc_aliases), FIELD_GROUP_IPC, enable_ipc},
		{"energy", energy_aliases, ARRAY_SIZE(energy_aliases), FIELD_GROUP_ENERGY, enable_energy},
	};
	char *arg_copy, *token, *saveptr = NULL;
	struct field_desc *fields = get_all_fields();
	int field_count = get_field_count();
	int all_selected = 0;

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

		if (strcmp(token, "all") == 0) {
			set_all_columns_enabled(enable);
			clear_field_overrides();
			if (enable)
				all_selected = 1;
			continue;
		}

		if (strcmp(token, "idle") == 0)
			clear_idle_state_overrides();

		int matched = 0;
		for (int i = 0; i < ARRAY_SIZE(groups); i++) {
			if (option_matches_alias_group(token, &groups[i])) {
				groups[i].set_visible(enable);
				if (!(enable && all_selected))
					set_group_field_overrides(groups[i].field_group_mask,
								 enable, enable);
				matched = 1;
				break;
			}
		}

		if (matched)
			continue;

		for (int i = 0; i < field_count; i++) {
			if (!field_matches_token(&fields[i], token))
				continue;

			/*
			 * Exact field tokens refine a group, they do not redefine the
			 * whole group.
			 *
			 * - Show/whitelist mode still needs the coarse group flag so the
			 *   data source gets sampled.
			 * - Hide/blacklist mode must not clear the group flag, otherwise
			 *   hiding a single field such as "uncore_freq" would also hide
			 *   its siblings (Freq/Min/Max/Governor/Boost).
			 */
			if (enable)
				*fields[i].enabled_ptr = 1;
			if (enable && fields[i].scope == FIELD_SCOPE_PACKAGE)
				enable_package(1);
			if (enable && fields[i].scope == FIELD_SCOPE_CPU)
				enable_cpu(1);
			if (!(enable && all_selected))
				set_field_override_by_index(i, enable, enable);
			matched = 1;

			if (fields[i].series == FIELD_SERIES_IDLE_STATE &&
			    fields[i].series_index >= 0) {
				if (enable)
					enable_cpuidle(1);
				set_idle_state_override(fields[i].series_index,
							enable, enable);
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
	int energy_requested = 0;
	int show_option_seen = 0;

	set_cpu_inventory_filter(NULL);
	set_section_summary_mode(0);
	set_section_default_summary_output(1);

	while ((opt = getopt_long(argc, argv, "i:n:N:c:B:o:O:qDSf:s:H:df:p:lJIPv?ah",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'i':
			if (parse_positive_double_arg("--interval", optarg,
						      &opts->interval) < 0)
				return -1;
			break;
		case 'n':
			if (parse_non_negative_int_arg("--num-iterations", optarg,
						       &opts->iterations) < 0)
				return -1;
			break;
		case 'N':
			if (parse_non_negative_int_arg("--header-iterations", optarg,
						       &opts->header_interval) < 0)
				return -1;
			set_text_header_interval(opts->header_interval);
			break;
		case 'q':
			opts->quiet = 1;
			set_text_quiet(1);
			break;
		case 'D':
			opts->dump_once = 1;
			break;
		case 'S':
			opts->summary_mode = 1;
			set_section_summary_mode(1);
			break;
		case 'a':
			set_all_columns_enabled(1);
			clear_field_overrides();
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
		case 'O':
			opts->output_file = optarg;
			break;
		case 's':
			if (apply_show_option(optarg, !show_option_seen) < 0)
				return -1;
			show_option_seen = 1;
			/* Independent metric requests remain in the whitelist union. */
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
			if (show_option_seen)
				set_group_field_overrides(FIELD_GROUP_ENERGY, 1, 1);
			break;
		case 'I':
			apply_ipc_option(opts);
			if (show_option_seen)
				set_group_field_overrides(FIELD_GROUP_IPC, 1, 1);
			break;
		case 'P':
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

	if (optind < argc) {
		fprintf(stderr, "Error: unexpected argument: %s\n", argv[optind]);
		return -1;
	}

	return 0;  /* Continue execution */
}

int apply_default_pmu_events(struct armstat_options *opts)
{
	/*
	 * If the user explicitly asked for PMU/IPC columns but did not provide
	 * an event list, default to the turbostat-like baseline pair so the
	 * columns are actionable instead of permanently unavailable.
	 */
	if (!opts->pmu_events && (is_pmu_enabled() || is_ipc_enabled()))
		opts->pmu_events = "cycles,instructions";

	/*
	 * If -I was used, ensure cycles,instructions are present in the event
	 * list so IPC can actually be computed.  Merge them in front of any
	 * existing -p events without duplicating.
	 */
	if (opts->ipc_requested && opts->pmu_events) {
		static char merged[512];
		int has_cycles = 0, has_instructions = 0;
		int written = 0;
		const char *p = opts->pmu_events;

		while (*p) {
			const char *start = p;
			const char *end = p;
			while (*end && *end != ',')
				end++;
			while (start < end && (*start == ' ' || *start == '\t'))
				start++;
			while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
				end--;
			if ((end - start) == 6 && strncmp(start, "cycles", 6) == 0)
				has_cycles = 1;
			if ((end - start) == 12 && strncmp(start, "instructions", 12) == 0)
				has_instructions = 1;
			p = (*end == ',') ? end + 1 : end;
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
