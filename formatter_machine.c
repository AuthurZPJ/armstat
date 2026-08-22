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
#include "formatter_section.h"
#include "aggregator.h"
#include "pmu.h"
#include "topology.h"
#include "cpu_inventory.h"

#define MACHINE_SCHEMA_VERSION 7

/* Machine formatter state */
static int json_first_interval = 1;
static int csv_header_printed = 0;

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
			if (!isfinite(value))
				printf("null");
			else
				printf("%.*f", field->decimals, value);
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
		if (!value)
			printf("null");
		else
			print_json_escaped_string(value);
		break;
	}
	case FIELD_TYPE_BOOL: {
		int value = field->getter.get_int(rec, cpu_idx);

		if (value < 0)
			printf("null");
		else
			printf("%s", value ? "true" : "false");
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

static void print_pmu_json_summary_object(const struct interval_record *rec)
{
	int pmu_count = rec->pmu_event_count;

	if (pmu_count <= 0 || !pmu_is_active() || !rec->summary.pmu_valid) {
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

	if (pmu_count <= 0 || !pmu_is_active() ||
	    !rec->cpu_rows[cpu_idx].pmu_valid) {
		printf("null");
		return;
	}

	printf("{");
	for (int i = 0; i < pmu_count; i++) {
		const char *name = get_pmu_event_name(i);
		printf("%s", i ? ", " : "");
		print_json_escaped_string(name ? name : "event");
		printf(": %llu", rec->cpu_rows[cpu_idx].pmu[i]);
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
		if (!pmu_is_active() || !rec->cpu_rows[cpu_idx].pmu_valid)
			printf(",");
		else
			printf(",%llu", rec->cpu_rows[cpu_idx].pmu[i]);
	}
}

static void print_pmu_csv_summary_values(const struct interval_record *rec)
{
	for (int i = 0; i < rec->pmu_event_count; i++) {
		if (!pmu_is_active() || !rec->summary.pmu_valid)
			printf(",");
		else
			printf(",%llu", rec->summary.pmu[i]);
	}
}

static unsigned long long record_timestamp_ns(const struct interval_record *rec)
{
	if (rec->timestamp_ns)
		return rec->timestamp_ns;
	return (unsigned long long)rec->timestamp * 1000000000ULL;
}

static void format_timestamp_iso(const struct interval_record *rec,
				 char *buf, size_t buf_size)
{
	struct tm tm;
	char date[32];
	char zone[16];
	char raw_zone[16];
	unsigned long long timestamp_ns;
	int written;

	if (!buf_size)
		return;

	if (!localtime_r(&rec->timestamp, &tm)) {
		buf[0] = '\0';
		return;
	}

	if (strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm) == 0 ||
	    strftime(raw_zone, sizeof(raw_zone), "%z", &tm) == 0) {
		buf[0] = '\0';
		return;
	}
	if (strlen(raw_zone) == 5 &&
	    (raw_zone[0] == '+' || raw_zone[0] == '-')) {
		snprintf(zone, sizeof(zone), "%c%c%c:%c%c", raw_zone[0],
			 raw_zone[1], raw_zone[2], raw_zone[3], raw_zone[4]);
	} else {
		snprintf(zone, sizeof(zone), "%s", raw_zone);
	}

	timestamp_ns = record_timestamp_ns(rec);
	written = snprintf(buf, buf_size, "%s.%09llu%s", date,
			   timestamp_ns % 1000000000ULL, zone);
	if (written < 0 || (size_t)written >= buf_size)
		buf[0] = '\0';
}

static void print_csv_metadata_header(void)
{
	printf("schema_version,interval,duration_us,timestamp,timestamp_ns,timestamp_iso,");
}

static void print_csv_metadata_prefix(const struct interval_record *rec)
{
	char ts_iso[64];

	format_timestamp_iso(rec, ts_iso, sizeof(ts_iso));
	printf("%d,%d,%llu,%ld,%llu,",
	       MACHINE_SCHEMA_VERSION,
	       rec->interval,
	       rec->duration_us,
	       (long)rec->timestamp,
	       record_timestamp_ns(rec));
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

static void get_serialized_fields(enum field_scope scope,
				  struct field_desc **fields, int *count)
{
	int out = 0;

	get_enabled_fields(scope, fields, count);
	for (int i = 0; i < *count; i++) {
		if (!field_is_scope_identity(fields[i]))
			fields[out++] = fields[i];
	}
	*count = out;
}

/* ============================================================================
 * SECTION 2: JSON SERIALIZER
 * ============================================================================ */

void serialize_json(const struct interval_record *rec, int iteration)
{
	struct field_desc *cpu_fields[64];
	struct field_desc *system_fields[64];
	int emit_cpu_section = !section_is_summary_mode() && section_emit_cpu();
	int emit_summary_section = section_is_summary_mode() || section_emit_default_summary();
	int emit_package_section = !section_is_summary_mode() &&
				   section_emit_package() &&
				   rec->package_count > 0;
	int cpu_field_count = 0;
	int system_field_count = 0;
	char ts_iso[64];

	struct field_desc *pkg_fields[64];
	int pkg_field_count = 0;
	get_serialized_fields(FIELD_SCOPE_PACKAGE, pkg_fields, &pkg_field_count);

	if (emit_cpu_section)
		get_serialized_fields(FIELD_SCOPE_CPU, cpu_fields, &cpu_field_count);
	if (emit_summary_section)
		get_serialized_fields(FIELD_SCOPE_SYSTEM, system_fields,
				      &system_field_count);

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
	printf("    \"duration_us\": %llu,\n", rec->duration_us);
	printf("    \"timestamp\": %ld,\n", rec->timestamp);
	printf("    \"timestamp_ns\": %llu,\n", record_timestamp_ns(rec));
	format_timestamp_iso(rec, ts_iso, sizeof(ts_iso));
	printf("    \"timestamp_iso\": ");
	print_json_escaped_string(ts_iso);
	printf("%s\n", (emit_package_section || emit_cpu_section ||
			 emit_summary_section) ? "," : "");

		/* Package array - in default mode */
		if (emit_package_section) {
			printf("    \"packages\": [\n");
			for (int pkg = 0; pkg < rec->package_count; pkg++) {
				int package_id = rec->packages[pkg].package_id;

				printf("      {\"package\": %d", package_id);
				for (int j = 0; j < pkg_field_count; j++)
					print_json_inline_field(pkg_fields[j], rec, pkg);
				printf("}%s\n", (pkg < rec->package_count - 1) ? "," : "");
		}
		printf("    ]%s\n", emit_cpu_section || emit_summary_section ? "," : "");
	}

	/* CPU array - only in default mode */
	if (emit_cpu_section) {
		printf("    \"cpus\": [\n");
		for (int i = 0; i < rec->cpu_row_count; i++) {
			int cpu_idx = rec->cpu_rows[i].cpu_idx;
			int cpu_id = get_cpu_row_id(rec, i);
			int is_last = (i == rec->cpu_row_count - 1);

			printf("      {\"cpu\": %d", cpu_id);

			/* Iterate through CPU fields - print directly */
			for (int j = 0; j < cpu_field_count; j++)
				print_json_inline_field(cpu_fields[j], rec, cpu_idx);

			if (is_pmu_enabled()) {
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
		if (is_pmu_enabled()) {
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
	struct field_desc *package_fields[64];
	struct field_desc *cpu_fields[64];
	int system_count = 0;
	int package_count = 0;
	int cpu_count = 0;
	int summary_section = section_is_summary_mode() ||
			      section_emit_default_summary();
	int package_section = !section_is_summary_mode() &&
			      section_emit_package();
	int cpu_section = !section_is_summary_mode() && section_emit_cpu();
	int mixed_scope = section_emit_mixed_csv();

	if (mixed_scope) {
		if (summary_section)
			get_serialized_fields(FIELD_SCOPE_SYSTEM, system_fields,
					   &system_count);
		if (package_section)
			get_serialized_fields(FIELD_SCOPE_PACKAGE, package_fields,
					   &package_count);
		if (cpu_section)
			get_serialized_fields(FIELD_SCOPE_CPU, cpu_fields, &cpu_count);

		print_csv_metadata_header();
		printf("Scope,CPU,Package");
		for (int i = 0; i < system_count; i++) {
			putchar(',');
			print_mixed_scope_csv_field_name("summary", system_fields[i]);
		}
		for (int i = 0; i < package_count; i++) {
			putchar(',');
			print_mixed_scope_csv_field_name("package", package_fields[i]);
		}
		for (int i = 0; i < cpu_count; i++) {
			putchar(',');
			print_mixed_scope_csv_field_name("cpu", cpu_fields[i]);
		}
		if (is_pmu_enabled()) {
			if (summary_section)
				print_prefixed_pmu_csv_headers("summary");
			if (cpu_section)
				print_prefixed_pmu_csv_headers("cpu");
		}
		printf("\n");
		return;
	}

	/* Print header */
	print_csv_metadata_header();

	int first = 1;
	if (summary_section) {
		get_serialized_fields(FIELD_SCOPE_SYSTEM, system_fields, &system_count);
		printf("Scope");
		first = 0;
		for (int i = 0; i < system_count; i++) {
			printf("%s", first ? "" : ",");
			print_csv_cell(system_fields[i]->label);
			first = 0;
		}
	} else if (package_section) {
		get_serialized_fields(FIELD_SCOPE_PACKAGE, package_fields,
				   &package_count);
		printf("Package");
		first = 0;
		for (int i = 0; i < package_count; i++) {
			printf("%s", first ? "" : ",");
			print_csv_cell(package_fields[i]->label);
			first = 0;
		}
	} else if (cpu_section && section_emit_cpu_identity()) {
		get_serialized_fields(FIELD_SCOPE_CPU, cpu_fields, &cpu_count);
		printf("CPU");
		first = 0;
		for (int i = 0; i < cpu_count; i++) {
			printf("%s", first ? "" : ",");
			print_csv_cell(cpu_fields[i]->label);
			first = 0;
		}
	}

	/* PMU data is defined at summary and CPU scope, not package scope. */
	if (is_pmu_enabled() && !package_section)
		print_pmu_csv_headers();
	printf("\n");
}

static void serialize_csv_row(const struct interval_record *rec, int row_idx)
{
	struct field_desc *fields[64];
	int count;

	int cpu_idx = rec->cpu_rows[row_idx].cpu_idx;
	int cpu_id = get_cpu_row_id(rec, row_idx);

	/* CPU column */
	print_csv_metadata_prefix(rec);

	int first = 1;
	if (section_emit_cpu_identity()) {
		printf("%d", cpu_id);
		first = 0;
	}

	/* Per-CPU fields */
	get_serialized_fields(FIELD_SCOPE_CPU, fields, &count);
	for (int i = 0; i < count; i++) {
		char tmp[64];
		format_field_value(fields[i], rec, cpu_idx, "", tmp, sizeof(tmp));

		printf("%s", first ? "" : ",");
		print_csv_cell(tmp);
		first = 0;
	}

	if (is_pmu_enabled())
		print_pmu_csv_cpu_values(rec, cpu_idx);

	printf("\n");
}

static void serialize_csv_package_row(const struct interval_record *rec,
				      int package_idx)
{
	struct field_desc *fields[64];
	int count;

	get_serialized_fields(FIELD_SCOPE_PACKAGE, fields, &count);
	print_csv_metadata_prefix(rec);
	printf("%d", rec->packages[package_idx].package_id);
	for (int i = 0; i < count; i++) {
		char tmp[64];

		format_field_value(fields[i], rec, package_idx, "", tmp,
				   sizeof(tmp));
		putchar(',');
		print_csv_cell(tmp);
	}
	printf("\n");
}

static void serialize_csv_mixed_cpu_row(const struct interval_record *rec,
					int row_idx, int system_count,
					int package_count,
					int summary_section)
{
	struct field_desc *cpu_fields[64];
	int cpu_count;

	int cpu_idx = rec->cpu_rows[row_idx].cpu_idx;
	int cpu_id = get_cpu_row_id(rec, row_idx);

	print_csv_metadata_prefix(rec);
	printf("CPU,%d,", cpu_id);
	print_empty_csv_cells(system_count);
	print_empty_csv_cells(package_count);

	get_serialized_fields(FIELD_SCOPE_CPU, cpu_fields, &cpu_count);
	for (int i = 0; i < cpu_count; i++) {
		char tmp[64];
		format_field_value(cpu_fields[i], rec, cpu_idx, "", tmp, sizeof(tmp));
		putchar(',');
		print_csv_cell(tmp);
	}

	if (is_pmu_enabled() && summary_section)
		print_empty_csv_cells(rec->pmu_event_count);

	if (is_pmu_enabled())
		print_pmu_csv_cpu_values(rec, cpu_idx);

	printf("\n");
}

static void serialize_csv_mixed_package_row(const struct interval_record *rec,
					    int package_idx,
					    int system_count,
					    int cpu_count,
					    int summary_section,
					    int cpu_section)
{
	struct field_desc *package_fields[64];
	int package_count;

	get_serialized_fields(FIELD_SCOPE_PACKAGE, package_fields, &package_count);
	print_csv_metadata_prefix(rec);
	printf("PKG,,%d", rec->packages[package_idx].package_id);
	print_empty_csv_cells(system_count);
	for (int i = 0; i < package_count; i++) {
		char tmp[64];

		format_field_value(package_fields[i], rec, package_idx, "", tmp,
				   sizeof(tmp));
		putchar(',');
		print_csv_cell(tmp);
	}
	print_empty_csv_cells(cpu_count);
	if (is_pmu_enabled() && summary_section)
		print_empty_csv_cells(rec->pmu_event_count);
	if (is_pmu_enabled() && cpu_section)
		print_empty_csv_cells(rec->pmu_event_count);
	printf("\n");
}

/*
 * Serialize CSV summary row
 */
static void serialize_csv_summary_row(const struct interval_record *rec)
{
	struct field_desc *fields[64];
	int count;

	get_serialized_fields(FIELD_SCOPE_SYSTEM, fields, &count);

	/* Print SUM label */
	print_csv_metadata_prefix(rec);
	printf("SUM");

	/* System fields */
	for (int i = 0; i < count; i++) {
		char tmp[64];
		format_field_value(fields[i], rec, 0, "", tmp, sizeof(tmp));

		putchar(',');
		print_csv_cell(tmp);
	}

	/* Aggregated PMU summary values */
	if (is_pmu_enabled())
		print_pmu_csv_summary_values(rec);

	printf("\n");
}

static void serialize_csv_mixed_summary_row(const struct interval_record *rec,
					    int package_count,
					    int cpu_count,
					    int cpu_section)
{
	struct field_desc *system_fields[64];
	int system_count;

	get_serialized_fields(FIELD_SCOPE_SYSTEM, system_fields, &system_count);

	print_csv_metadata_prefix(rec);
	printf("SUM,,");

	for (int i = 0; i < system_count; i++) {
		char tmp[64];
		format_field_value(system_fields[i], rec, 0, "", tmp, sizeof(tmp));
		putchar(',');
		print_csv_cell(tmp);
	}

	print_empty_csv_cells(package_count);
	print_empty_csv_cells(cpu_count);

	if (is_pmu_enabled())
		print_pmu_csv_summary_values(rec);

	if (is_pmu_enabled() && cpu_section)
		print_empty_csv_cells(rec->pmu_event_count);

	printf("\n");
}

void serialize_csv(const struct interval_record *rec)
{
	int summary_section = section_is_summary_mode() ||
			      section_emit_default_summary();
	int package_section = !section_is_summary_mode() &&
			      section_emit_package();
	int cpu_section = !section_is_summary_mode() && section_emit_cpu();
	int mixed_scope = section_emit_mixed_csv();

	/* Print header once */
	if (!csv_header_printed) {
		serialize_csv_header();
		csv_header_printed = 1;
	}

	if (mixed_scope) {
		struct field_desc *system_fields[64];
		struct field_desc *package_fields[64];
		struct field_desc *cpu_fields[64];
		int system_count = 0;
		int package_count = 0;
		int cpu_count = 0;

		if (summary_section)
			get_serialized_fields(FIELD_SCOPE_SYSTEM, system_fields,
					   &system_count);
		if (package_section)
			get_serialized_fields(FIELD_SCOPE_PACKAGE, package_fields,
					   &package_count);
		if (cpu_section)
			get_serialized_fields(FIELD_SCOPE_CPU, cpu_fields, &cpu_count);

		if (summary_section)
			serialize_csv_mixed_summary_row(rec, package_count,
						cpu_count, cpu_section);
		if (package_section) {
			for (int i = 0; i < rec->package_count; i++)
				serialize_csv_mixed_package_row(rec, i,
								system_count,
								cpu_count,
								summary_section,
								cpu_section);
		}
		if (cpu_section) {
			for (int i = 0; i < rec->cpu_row_count; i++)
				serialize_csv_mixed_cpu_row(rec, i, system_count,
							       package_count,
							       summary_section);
		}
		fflush(stdout);
		return;
	}

	if (summary_section) {
		serialize_csv_summary_row(rec);
	} else if (package_section) {
		for (int i = 0; i < rec->package_count; i++)
			serialize_csv_package_row(rec, i);
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

void reset_machine_state(void)
{
	json_first_interval = 1;
	csv_header_printed = 0;
}
