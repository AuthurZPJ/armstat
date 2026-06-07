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
	int summary_mode;
	int format;  /* FORMAT_TEXT, FORMAT_JSON, FORMAT_CSV */

	/* CPU filter */
	char *cpu_filter;
	char *busy_source;

	/* PMU */
	char *pmu_events;
	int ipc_requested;  /* Track if -I was used (requires PMU) */
	int ipc_column_requested;  /* Track if -s ipc was used (just shows column) */

	/* Other */
	int debug;
	int list_counters;
	int dump_once;
	int probe_only;
};

extern struct armstat_options default_options;

int parse_args(int argc, char *argv[], struct armstat_options *opts);
void apply_default_pmu_events(struct armstat_options *opts);

/* Exposed for regression tests — column selection logic */
void set_all_columns_enabled(int enable);
void parse_column_option(const char *arg, int enable);

#endif /* ARMSTAT_CLI_H */
