/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter.c - Output formatting layer
 *
 * This file is now a thin facade that delegates to specialized modules:
 *   - formatter_record.c: Build interval_record from raw data
 *   - formatter_text.c: Text serialization
 *   - formatter_machine.c: JSON/CSV serialization
 *
 * RESPONSIBLE FOR:
 *   - Public API entry points
 *   - Coordinating between sub-modules
 *
 * NOT RESPONSIBLE FOR:
 *   - Field table definition (formatter_record.c)
 *   - Text serialization logic (formatter_text.c)
 *   - JSON/CSV serialization logic (formatter_machine.c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "formatter.h"
#include "aggregator.h"

/* Internal wiring between the formatter facade and sub-modules */
void set_text_quiet(int quiet);
void set_machine_quiet(int quiet);
void set_text_summary_mode(int summary);
void set_machine_summary_mode(int summary);
void set_text_header_interval(int interval);
void set_text_default_summary_output(int enable);
void set_machine_default_summary_output(int enable);
void close_machine_json(void);

#include "pmu.h"
#include "topology.h"
#include "cpu_inventory.h"
#include "cpufreq.h"
#include "cpuidle.h"
#include "power.h"

/* ============================================================================
 * FORMATTER FACADE
 * ============================================================================ */

static int format_type = FORMAT_TEXT;
static int quiet_mode = 0;
static int summary_mode = 0;
static int header_interval = 0;
static int default_summary_output = 0;

/* ============================================================================
 * INCLUDES FOR DELEGATION
 * ============================================================================ */

/*
 * Formatter entry point (text serializer)
 * This deliberately includes the .c files to avoid exporting internal functions
 */
static void format_text_internal(const struct interval_record *rec, int iteration)
{
	extern void serialize_text(const struct interval_record *rec, int iteration);
	serialize_text(rec, iteration);
}

static void format_json_internal(const struct interval_record *rec, int iteration)
{
	extern void serialize_json(const struct interval_record *rec, int iteration);
	serialize_json(rec, iteration);
}

static void format_csv_internal(const struct interval_record *rec)
{
	extern void serialize_csv(const struct interval_record *rec);
	serialize_csv(rec);
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

void set_format(int format)
{
	format_type = format;
	if (format == FORMAT_JSON)
		quiet_mode = 1;  /* JSON doesn't need headers */
	set_machine_quiet(quiet_mode);
}

void set_quiet(int quiet)
{
	quiet_mode = quiet;
	set_text_quiet(quiet);
	set_machine_quiet(quiet);
}

void set_summary_mode(int summary)
{
	summary_mode = summary;
	set_text_summary_mode(summary);
	set_machine_summary_mode(summary);
}

void set_default_summary_output(int enable)
{
	default_summary_output = enable;

	set_text_default_summary_output(enable);
	set_machine_default_summary_output(enable);
}

void set_header_interval(int interval)
{
	header_interval = interval;
	set_text_header_interval(interval);
}

int init_formatter(void)
{
	/* Column config is set by parse_args() before init_modules() */
	if (show_idle)
		update_idle_state_visibility();
	update_temp_field_visibility();
	return 0;
}

void print_interval(const struct sys_snapshot *raw,
		    const struct interval_stats *stats,
		    int iteration)
{
	struct interval_record *rec = build_interval_record(raw, stats, iteration);

	if (!rec)
		return;

	switch (format_type) {
	case FORMAT_JSON:
		format_json_internal(rec, iteration);
		break;
	case FORMAT_CSV:
		format_csv_internal(rec);
		break;
	case FORMAT_TEXT:
	default:
		format_text_internal(rec, iteration);
		break;
	}

	free_interval_record(rec);
}

void print_interval_header(double interval)
{
	if (format_type == FORMAT_JSON || format_type == FORMAT_CSV)
		return;
	if (quiet_mode)
		return;
	printf("armstat - ARM Server Performance Monitor\n");
	printf("Sampling interval: %.1f second(s)\n", interval);
	printf("\n");
}

void list_counters(void)
{
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
	printf("Exact field names like Idle%%, Busy%%, LPI-0 are also accepted.\n");
	printf("Built-in PMU events:\n");
	list_builtin_pmu_events();
}

void cleanup_formatter(void)
{
	cleanup_formatter_pool();
}

void close_format(const struct interval_stats *stats)
{
	(void)stats;
	if (format_type == FORMAT_JSON)
		close_machine_json();
}
