/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_machine.c - Stage 2: Machine-readable serialization (JSON/CSV)
 *
 * Outputs JSON and CSV formats by iterating through the field table.
 * No serializer needs to know about individual show_* flags.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "formatter.h"
#include "aggregator.h"
#include "pmu.h"
#include "topology.h"
#include "cpu_inventory.h"

#define MACHINE_SCHEMA_VERSION 4

/* Machine formatter state */
static int summary_mode = 0;
static int json_first_interval = 1;
static int default_summary_output = 0;

static int should_emit_package_section(void)
{
	return any_fields_enabled(FIELD_SCOPE_PACKAGE);
}

static int should_emit_cpu_section(void)
{
	return show_cpu || show_pmu || any_fields_enabled(FIELD_SCOPE_CPU);
}

/*
 * CSV per-CPU rows need a stable row key, just like text and JSON.
 * Keep the CPU column whenever we emit per-CPU rows so the output remains
 * self-describing even if the user hid the cpu group.
 */
static int should_emit_cpu_identity(void)
{
	return should_emit_cpu_section();
}

static int should_emit_default_summary_section(void)
{
	return default_summary_output &&
	       any_fields_enabled(FIELD_SCOPE_SYSTEM) &&
	       !cpu_inventory_filter_is_active();
}

static int should_emit_mixed_csv_section(void)
{
	return !summary_mode &&
	       should_emit_cpu_section() &&
	       should_emit_default_summary_section();
}

/* ============================================================================
 * SECTION 1: MACHINE OUTPUT HELPERS
 * ============================================================================ */

static void print_json_escaped_string(const char *value)
{
	const unsigned char *p = (const unsigned char *)(value ? value : "");

	putchar('"');
	for (; *p; p++) {
		switch (*p) {
		case '\\':
			fputs("\\\\", stdout);
			break;
		case '"':
			fputs("\\\"", stdout);
			break;
		case '\b':
			fputs("\\b", stdout);
			break;
		case '\f':
			fputs("\\f", stdout);
			break;
		case '\n':
			fputs("\\n", stdout);
			break;
		case '\r':
			fputs("\\r", stdout);
			break;
		case '\t':
			fputs("\\t", stdout);
			break;
		default:
			if (*p < 0x20)
				printf("\\u%04x", *p);
			else
				putchar(*p);
			break;
		}
	}
	putchar('"');
}

static int csv_needs_quotes(const char *value)
{
	if (!value || !*value)
		return 0;

	for (const char *p = value; *p; p++) {
		if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r')
			return 1;
	}

	return 0;
}

static void print_csv_cell(const char *value)
{
	const char *p = value ? value : "";

	if (!csv_needs_quotes(p)) {
		fputs(p, stdout);
		return;
	}

	putchar('"');
	for (; *p; p++) {
		if (*p == '"')
			putchar('"');
		putchar(*p);
	}
	putchar('"');
}

static void print_json_field_value(const struct field_desc *field,
				   const struct interval_record *rec,
				   int cpu_idx)
{
	switch (field->type) {
	case FIELD_TYPE_DOUBLE:
		{
			double value = field->getter.get_double(rec, cpu_idx);
			if (isnan(value))
				printf("null");
			else
				printf("%.2f", value);
		}
		break;
	case FIELD_TYPE_LLONG:
		printf("%lld", field->getter.get_llong(rec, cpu_idx));
		break;
	case FIELD_TYPE_INT:
		printf("%d", field->getter.get_int(rec, cpu_idx));
		break;
	case FIELD_TYPE_STRING: {
		const char *value = field->getter.get_string(rec, cpu_idx);
		print_json_escaped_string(value);
		break;
	}
	}
}

static void print_json_inline_field(const struct field_desc *field,
				    const struct interval_record *rec,
				    int cpu_idx)
{
	printf(", ");
	print_json_escaped_string(field->json_label);
	printf(": ");
	print_json_field_value(field, rec, cpu_idx);
}

static void print_json_multiline_field(const struct field_desc *field,
				       const struct interval_record *rec,
				       int cpu_idx,
				       int *needs_comma)
{
	printf("%s      ", *needs_comma ? ",\n" : "");
	print_json_escaped_string(field->json_label);
	printf(": ");
	print_json_field_value(field, rec, cpu_idx);
	*needs_comma = 1;
}

static void format_field_value(const struct field_desc *field,
			       const struct interval_record *rec,
			       int cpu_idx,
			       char *buf,
			       size_t buf_size)
{
	switch (field->type) {
	case FIELD_TYPE_DOUBLE: {
		double value = field->getter.get_double(rec, cpu_idx);
		if (isnan(value))
			buf[0] = '\0';
		else
			snprintf(buf, buf_size, "%.2f", value);
		break;
	}
	case FIELD_TYPE_LLONG:
		snprintf(buf, buf_size, "%lld", field->getter.get_llong(rec, cpu_idx));
		break;
	case FIELD_TYPE_INT:
		snprintf(buf, buf_size, "%d", field->getter.get_int(rec, cpu_idx));
		break;
	case FIELD_TYPE_STRING: {
		const char *value = field->getter.get_string(rec, cpu_idx);
		snprintf(buf, buf_size, "%s", value ? value : "");
		break;
	}
	default:
		buf[0] = '\0';
		break;
	}
}

static void print_pmu_json_summary_object(const struct interval_record *rec)
{
	int pmu_count = rec->pmu_event_count;

	if (pmu_count <= 0 || !pmu_is_active()) {
		printf("null");
		return;
	}

	printf("{\n");
	for (int i = 0; i < pmu_count; i++) {
		const char *name = get_pmu_event_name(i);
		printf("        ");
		print_json_escaped_string(name ? name : "event");
		printf(": %llu%s\n",
		       rec->summary.pmu[i],
		       i < pmu_count - 1 ? "," : "");
	}
	printf("      }");
}

static void print_pmu_json_cpu_object(const struct interval_record *rec, int cpu_idx)
{
	int pmu_count = rec->pmu_event_count;

	if (pmu_count <= 0 || !rec->stats || !pmu_is_active()) {
		printf("null");
		return;
	}

	printf("{");
	for (int i = 0; i < pmu_count; i++) {
		const char *name = get_pmu_event_name(i);
		printf("%s", i ? ", " : "");
		print_json_escaped_string(name ? name : "event");
		printf(": %llu", rec->stats->per_cpu_pmu[cpu_idx][i]);
	}
	printf("}");
}

static void print_pmu_csv_headers(void)
{
	int pmu_count = get_pmu_event_count();

	for (int i = 0; i < pmu_count; i++) {
		const char *name = get_pmu_event_name(i);

		putchar(',');
		print_csv_cell(name ? name : "pmu");
	}
}

static void print_prefixed_pmu_csv_headers(const char *scope_prefix)
{
	int pmu_count = get_pmu_event_count();
	char header[128];

	for (int i = 0; i < pmu_count; i++) {
		const char *name = get_pmu_event_name(i);

		putchar(',');
		snprintf(header, sizeof(header), "%s.pmu.%s",
			 scope_prefix, name ? name : "pmu");
		print_csv_cell(header);
	}
}

static void print_pmu_csv_cpu_values(const struct interval_record *rec, int cpu_idx)
{
	for (int i = 0; i < rec->pmu_event_count; i++) {
		if (!rec->stats || !pmu_is_active())
			printf(",");
		else
			printf(",%llu", rec->stats->per_cpu_pmu[cpu_idx][i]);
	}
}

static void print_pmu_csv_summary_values(const struct interval_record *rec)
{
	for (int i = 0; i < rec->pmu_event_count; i++) {
		if (!pmu_is_active())
			printf(",");
		else
			printf(",%llu", rec->summary.pmu[i]);
	}
}

static void format_timestamp_iso(time_t timestamp, char *buf, size_t buf_size)
{
	struct tm tm;

	if (!buf_size)
		return;

	if (!localtime_r(&timestamp, &tm)) {
		buf[0] = '\0';
		return;
	}

	if (strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%S%z", &tm) == 0)
		buf[0] = '\0';
}

static void print_csv_metadata_header(void)
{
	printf("schema_version,interval,timestamp,timestamp_iso,");
}

static void print_csv_metadata_prefix(const struct interval_record *rec)
{
	char ts_iso[64];

	format_timestamp_iso(rec->timestamp, ts_iso, sizeof(ts_iso));
	printf("%d,%d,%ld,",
	       MACHINE_SCHEMA_VERSION,
	       rec->interval,
	       (long)rec->timestamp);
	print_csv_cell(ts_iso);
	putchar(',');
}

static void print_empty_csv_cells(int count)
{
	for (int i = 0; i < count; i++)
		putchar(',');
}

static void print_mixed_scope_csv_field_name(const char *scope_prefix,
					     const struct field_desc *field)
{
	char name[128];
	const char *base = (field && field->json_label) ? field->json_label : "field";

	snprintf(name, sizeof(name), "%s.%s", scope_prefix, base);
	print_csv_cell(name);
}

/* ============================================================================
 * SECTION 2: JSON SERIALIZER
 * ============================================================================ */

void serialize_json(const struct interval_record *rec, int iteration)
{
	struct field_desc *cpu_fields[64];
	struct field_desc *system_fields[64];
	int emit_cpu_section = !summary_mode && should_emit_cpu_section();
	int emit_summary_section = summary_mode || should_emit_default_summary_section();
	int emit_package_section = !summary_mode &&
				   should_emit_package_section() &&
				   rec->stats && rec->stats->package_count > 0;
	int cpu_field_count = 0;
	int system_field_count = 0;
	char ts_iso[64];

	struct field_desc *pkg_fields[64];
	int pkg_field_count = 0;
	get_enabled_fields(FIELD_SCOPE_PACKAGE, pkg_fields, &pkg_field_count);

	if (emit_cpu_section)
		get_enabled_fields(FIELD_SCOPE_CPU, cpu_fields, &cpu_field_count);
	if (emit_summary_section)
		get_enabled_fields(FIELD_SCOPE_SYSTEM, system_fields, &system_field_count);

	/* Print opening bracket for first interval */
	if (json_first_interval) {
		printf("[\n");
		json_first_interval = 0;
	} else {
		printf(",\n");
	}

	printf("  {\n");
	printf("    \"schema_version\": %d,\n", MACHINE_SCHEMA_VERSION);
	printf("    \"interval\": %d,\n", iteration);
	printf("    \"timestamp\": %ld,\n", rec->timestamp);
	format_timestamp_iso(rec->timestamp, ts_iso, sizeof(ts_iso));
	printf("    \"timestamp_iso\": ");
	print_json_escaped_string(ts_iso);
	printf("%s\n", (emit_package_section || emit_cpu_section ||
			 emit_summary_section) ? "," : "");

		/* Package array - in default mode */
		if (emit_package_section) {
			printf("    \"packages\": [\n");
			for (int pkg = 0; pkg < rec->stats->package_count; pkg++) {
				int package_id = rec->stats->packages[pkg].package_id;

				printf("      {\"package\": %d", package_id);
				for (int j = 0; j < pkg_field_count; j++)
					print_json_inline_field(pkg_fields[j], rec, pkg);
				printf("}%s\n", (pkg < rec->stats->package_count - 1) ? "," : "");
		}
		printf("    ]%s\n", emit_cpu_section || emit_summary_section ? "," : "");
	}

	/* CPU array - only in default mode */
	if (emit_cpu_section) {
		printf("    \"cpus\": [\n");
		for (int i = 0; i < rec->cpu_row_count; i++) {
			int cpu_idx = rec->cpu_rows[i].cpu_idx;
			int cpu_id = get_cpu_id_by_tracked_idx(cpu_idx);
			int is_last = (i == rec->cpu_row_count - 1);

			printf("      {\"cpu\": %d", cpu_id);

			/* Iterate through CPU fields - print directly */
			for (int j = 0; j < cpu_field_count; j++)
				print_json_inline_field(cpu_fields[j], rec, cpu_idx);

			if (show_pmu) {
				printf(", \"pmu\": ");
				print_pmu_json_cpu_object(rec, cpu_idx);
			}

			printf("}%s\n", is_last ? "" : ",");
		}
		printf("    ]%s\n",
		       emit_summary_section ? "," : "");
	}

	/* Summary object - in summary mode or explicit mixed-scope text/json mode */
	if (emit_summary_section) {
		int needs_comma = 0;

		printf("    \"summary\": {\n");

		for (int i = 0; i < system_field_count; i++)
			print_json_multiline_field(system_fields[i], rec, 0,
						   &needs_comma);

		/* PMU events (aggregated summary) */
		if (show_pmu) {
			printf("%s      \"pmu\": ",
			       needs_comma ? ",\n" : "");
			print_pmu_json_summary_object(rec);
			needs_comma = 1;
		}

		if (needs_comma)
			printf("\n");
		printf("    }\n");
	}

	printf("  }");
}

void close_machine_json(void)
{
	if (json_first_interval)
		printf("[\n]\n");
	else
		printf("\n]\n");

	json_first_interval = 1;
}

/* ============================================================================
 * SECTION 3: CSV SERIALIZER
 * ============================================================================ */

static void serialize_csv_header(void)
{
	struct field_desc *system_fields[64];
	struct field_desc *cpu_fields[64];
	int system_count;
	int cpu_count;
	int cpu_section = should_emit_cpu_section();
	int system_section = any_fields_enabled(FIELD_SCOPE_SYSTEM);
	int mixed_scope = should_emit_mixed_csv_section();

	if (mixed_scope) {
		get_enabled_fields(FIELD_SCOPE_SYSTEM, system_fields, &system_count);
		get_enabled_fields(FIELD_SCOPE_CPU, cpu_fields, &cpu_count);

		print_csv_metadata_header();
		printf("Scope,CPU");
		for (int i = 0; i < system_count; i++) {
			putchar(',');
			print_mixed_scope_csv_field_name("summary", system_fields[i]);
		}
		for (int i = 0; i < cpu_count; i++) {
			putchar(',');
			print_mixed_scope_csv_field_name("cpu", cpu_fields[i]);
		}
		if (show_pmu) {
			print_prefixed_pmu_csv_headers("summary");
			print_prefixed_pmu_csv_headers("cpu");
		}
		printf("\n");
		return;
	}

	/* In summary mode, or when only system fields remain, use system fields */
	enum field_scope scope = (summary_mode || (!cpu_section && system_section)) ?
		FIELD_SCOPE_SYSTEM : FIELD_SCOPE_CPU;
	get_enabled_fields(scope, scope == FIELD_SCOPE_SYSTEM ? system_fields : cpu_fields,
			   scope == FIELD_SCOPE_SYSTEM ? &system_count : &cpu_count);

	/* Print header */
	print_csv_metadata_header();

	int first = 1;
	if (scope == FIELD_SCOPE_SYSTEM) {
		printf("SUM");
		first = 0;
		for (int i = 0; i < system_count; i++) {
			printf("%s", first ? "" : ",");
			print_csv_cell(system_fields[i]->label);
			first = 0;
		}
	} else if (should_emit_cpu_identity()) {
		printf("CPU");
		first = 0;
		for (int i = 0; i < cpu_count; i++) {
			printf("%s", first ? "" : ",");
			print_csv_cell(cpu_fields[i]->label);
			first = 0;
		}
	}

	/* PMU columns */
	if (show_pmu)
		print_pmu_csv_headers();
	printf("\n");
}

static void serialize_csv_row(const struct interval_record *rec, int row_idx)
{
	struct field_desc *fields[64];
	int count;

	int cpu_idx = rec->cpu_rows[row_idx].cpu_idx;
	int cpu_id = get_cpu_id_by_tracked_idx(cpu_idx);

	/* CPU column */
	print_csv_metadata_prefix(rec);

	int first = 1;
	if (should_emit_cpu_identity()) {
		printf("%d", cpu_id);
		first = 0;
	}

	/* Per-CPU fields */
	get_enabled_fields(FIELD_SCOPE_CPU, fields, &count);
	for (int i = 0; i < count; i++) {
		char tmp[64];
		format_field_value(fields[i], rec, cpu_idx, tmp, sizeof(tmp));

		printf("%s", first ? "" : ",");
		print_csv_cell(tmp);
		first = 0;
	}

	if (show_pmu)
		print_pmu_csv_cpu_values(rec, cpu_idx);

	printf("\n");
}

static void serialize_csv_mixed_cpu_row(const struct interval_record *rec, int row_idx,
					int system_count)
{
	struct field_desc *cpu_fields[64];
	int cpu_count;

	int cpu_idx = rec->cpu_rows[row_idx].cpu_idx;
	int cpu_id = get_cpu_id_by_tracked_idx(cpu_idx);

	print_csv_metadata_prefix(rec);
	printf("CPU,%d", cpu_id);
	print_empty_csv_cells(system_count);

	get_enabled_fields(FIELD_SCOPE_CPU, cpu_fields, &cpu_count);
	for (int i = 0; i < cpu_count; i++) {
		char tmp[64];
		format_field_value(cpu_fields[i], rec, cpu_idx, tmp, sizeof(tmp));
		putchar(',');
		print_csv_cell(tmp);
	}

	if (show_pmu)
		print_empty_csv_cells(rec->pmu_event_count);

	if (show_pmu)
		print_pmu_csv_cpu_values(rec, cpu_idx);

	printf("\n");
}

/*
 * Serialize CSV summary row
 */
static void serialize_csv_summary_row(const struct interval_record *rec)
{
	struct field_desc *fields[64];
	int count;

	get_enabled_fields(FIELD_SCOPE_SYSTEM, fields, &count);

	/* Print SUM label */
	print_csv_metadata_prefix(rec);
	printf("SUM");

	/* System fields */
	for (int i = 0; i < count; i++) {
		char tmp[64];
		format_field_value(fields[i], rec, 0, tmp, sizeof(tmp));

		putchar(',');
		print_csv_cell(tmp);
	}

	/* Aggregated PMU summary values */
	if (show_pmu)
		print_pmu_csv_summary_values(rec);

	printf("\n");
}

static void serialize_csv_mixed_summary_row(const struct interval_record *rec,
					    int cpu_count)
{
	struct field_desc *system_fields[64];
	int system_count;

	get_enabled_fields(FIELD_SCOPE_SYSTEM, system_fields, &system_count);

	print_csv_metadata_prefix(rec);
	printf("SUM,");

	for (int i = 0; i < system_count; i++) {
		char tmp[64];
		format_field_value(system_fields[i], rec, 0, tmp, sizeof(tmp));
		if (i > 0)
			putchar(',');
		print_csv_cell(tmp);
	}

	print_empty_csv_cells(cpu_count);

	if (show_pmu)
		print_pmu_csv_summary_values(rec);

	if (show_pmu)
		print_empty_csv_cells(rec->pmu_event_count);

	printf("\n");
}

void serialize_csv(const struct interval_record *rec)
{
	static int csv_header_printed;
	int cpu_section = should_emit_cpu_section();
	int system_section = any_fields_enabled(FIELD_SCOPE_SYSTEM);
	int mixed_scope = should_emit_mixed_csv_section();

	/* Print header once */
	if (!csv_header_printed) {
		serialize_csv_header();
		csv_header_printed = 1;
	}

	if (mixed_scope) {
		struct field_desc *system_fields[64];
		struct field_desc *cpu_fields[64];
		int system_count;
		int cpu_count;

		get_enabled_fields(FIELD_SCOPE_SYSTEM, system_fields, &system_count);
		get_enabled_fields(FIELD_SCOPE_CPU, cpu_fields, &cpu_count);

		serialize_csv_mixed_summary_row(rec, cpu_count);
		for (int i = 0; i < rec->cpu_row_count; i++)
			serialize_csv_mixed_cpu_row(rec, i, system_count);
		fflush(stdout);
		return;
	}

	/* Print summary row in summary mode, otherwise per-CPU rows */
	if (summary_mode || (!cpu_section && system_section)) {
		serialize_csv_summary_row(rec);
	} else if (cpu_section) {
		for (int i = 0; i < rec->cpu_row_count; i++) {
			serialize_csv_row(rec, i);
		}
	}

	fflush(stdout);
}

/* ============================================================================
 * SECTION 4: MACHINE FORMAT CONFIG
 * ============================================================================ */

void set_machine_quiet(int quiet)
{
	/*
	 * Kept for symmetry with the text formatter. JSON/CSV serializers
	 * currently do not use a separate quiet-mode concept.
	 */
	(void)quiet;
}

void set_machine_summary_mode(int summary)
{
	summary_mode = summary;
}

void set_machine_default_summary_output(int enable)
{
	default_summary_output = enable;
}

void reset_machine_state(void)
{
	json_first_interval = 1;
}
