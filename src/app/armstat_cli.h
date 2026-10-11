/* SPDX-License-Identifier: GPL-2.0 */
/*
 * armstat_cli.h - Command-line interface shared types and declarations
 *
 * Separated from armstat.c so the column-selection tests can link against
 * the CLI layer without pulling in the main event loop.
 */
#ifndef ARMSTAT_CLI_H
#define ARMSTAT_CLI_H

#define ARRAY_SIZE(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

struct armstat_options {
	/* Timing */
	double interval;
	int iterations;
	int header_interval;

	/* Output */
	char *output_file;
	int quiet;
	int quiet_all;
	int summary_mode;
	int format;  /* FORMAT_TEXT, FORMAT_JSON, FORMAT_CSV, FORMAT_NDJSON */

	/* CPU filter */
	char *cpu_filter;
	char *busy_source;

	/* PMU */
	char *pmu_events;
	int ipc_requested;  /* Track if -I was used (requires PMU) */
	int pmu_auto;       /* Best-effort IPC collection without raw PMU columns */
	int no_perf;        /* Disable perf-backed PMU collection */

	/* Other */
	int debug;
	int list_counters;
	int dump_raw;
	int probe_only;
	char **command_argv; /* argv after the explicit -- command separator */
};

extern struct armstat_options default_options;

int parse_args(int argc, char *argv[], struct armstat_options *opts);
int apply_default_pmu_events(struct armstat_options *opts);
/* Best-effort startup; callback is normally init_pmu_events(). */
int initialize_selected_pmu(const struct armstat_options *opts,
			    int (*init_events)(const char *events));

/* Exposed for regression tests — column selection logic */
void set_all_columns_enabled(int enable);
int parse_column_option(const char *arg, int enable);

/*
 * Startup banner for text mode; suppressed for machine formats or -q.
 * Implemented in armstat_cli.c.
 */
void print_interval_header(const struct armstat_options *opts, double interval);

/* List available column groups and PMU events. Implemented in armstat_cli.c. */
void list_counters(void);

#endif /* ARMSTAT_CLI_H */
