/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_csv.c - CSV stream serialization
 */

#include <stdio.h>
#include <string.h>

#include "formatter_machine.h"
#include "formatter_section.h"
#include "pmu.h"

static int csv_header_printed;

struct csv_layout {
	struct field_desc *fields[3][MACHINE_FIELD_CAPACITY];
	int counts[3];
	int sections[3];
	int cpu_identity;
	int pmu_count;
	int mixed;
};

static struct csv_layout initial_csv_layout;

static void get_csv_layout(struct csv_layout *layout)
{
	const enum field_scope scopes[] = {
		FIELD_SCOPE_SYSTEM, FIELD_SCOPE_PACKAGE, FIELD_SCOPE_CPU
	};

	memset(layout, 0, sizeof(*layout));
	layout->sections[0] = section_is_summary_mode() ||
		section_emit_default_summary();
	layout->sections[1] = !section_is_summary_mode() && section_emit_package();
	layout->sections[2] = !section_is_summary_mode() && section_emit_cpu();
	layout->cpu_identity = layout->sections[2] && section_emit_cpu_identity();
	layout->pmu_count = is_pmu_enabled() ? get_pmu_event_count() : 0;
	layout->mixed = section_emit_mixed_csv();
	for (int i = 0; i < 3; i++) {
		if (layout->sections[i])
			machine_get_serialized_fields(scopes[i], layout->fields[i],
						      &layout->counts[i]);
	}
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

static void print_pmu_csv_headers(void)
{
	char header[128];

	for (int i = 0; i < get_pmu_event_count(); i++) {
		const char *name = get_pmu_event_name(i);

		putchar(',');
		snprintf(header, sizeof(header), "pmu.%s", name ? name : "event");
		print_csv_cell(header);
	}
}

static void print_prefixed_pmu_csv_headers(const char *scope_prefix)
{
	char header[128];

	for (int i = 0; i < get_pmu_event_count(); i++) {
		const char *name = get_pmu_event_name(i);

		putchar(',');
		snprintf(header, sizeof(header), "%s.pmu.%s", scope_prefix,
			 name ? name : "pmu");
		print_csv_cell(header);
	}
}

static void print_pmu_csv_cpu_values(const struct interval_record *rec,
				     int row_idx)
{
	for (int i = 0; i < rec->pmu_event_count; i++) {
		if (!pmu_is_active() || !rec->cpu_rows[row_idx].pmu_valid)
			printf(",");
		else
			printf(",%llu", rec->cpu_rows[row_idx].pmu[i]);
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

static void print_csv_metadata_header(void)
{
	printf("schema_version,interval,duration_us,timestamp,timestamp_ns,timestamp_iso,");
}

static void print_csv_metadata_prefix(const struct interval_record *rec)
{
	char timestamp_iso[64];

	machine_format_timestamp_iso(rec, timestamp_iso, sizeof(timestamp_iso));
	printf("%d,%llu,%llu,%ld,%llu,", MACHINE_SCHEMA_VERSION, rec->interval,
	       rec->duration_us, (long)rec->timestamp,
	       machine_record_timestamp_ns(rec));
	print_csv_cell(timestamp_iso);
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
	const char *base = field && field->json_label ?
		field->json_label : "field";

	snprintf(name, sizeof(name), "%s.%s", scope_prefix, base);
	print_csv_cell(name);
}

static void serialize_csv_mixed_header(int summary_section,
				       int package_section, int cpu_section)
{
	struct field_desc *system_fields[MACHINE_FIELD_CAPACITY];
	struct field_desc *package_fields[MACHINE_FIELD_CAPACITY];
	struct field_desc *cpu_fields[MACHINE_FIELD_CAPACITY];
	int system_count = 0;
	int package_count = 0;
	int cpu_count = 0;

	if (summary_section)
		machine_get_serialized_fields(FIELD_SCOPE_SYSTEM, system_fields,
					      &system_count);
	if (package_section)
		machine_get_serialized_fields(FIELD_SCOPE_PACKAGE, package_fields,
					      &package_count);
	if (cpu_section)
		machine_get_serialized_fields(FIELD_SCOPE_CPU, cpu_fields,
					      &cpu_count);

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
}

static void serialize_csv_header(void)
{
	struct field_desc *system_fields[MACHINE_FIELD_CAPACITY];
	struct field_desc *package_fields[MACHINE_FIELD_CAPACITY];
	struct field_desc *cpu_fields[MACHINE_FIELD_CAPACITY];
	int system_count = 0;
	int package_count = 0;
	int cpu_count = 0;
	int summary_section = section_is_summary_mode() ||
		section_emit_default_summary();
	int package_section = !section_is_summary_mode() &&
		section_emit_package();
	int cpu_section = !section_is_summary_mode() && section_emit_cpu();

	if (section_emit_mixed_csv()) {
		serialize_csv_mixed_header(summary_section, package_section,
					   cpu_section);
		return;
	}

	print_csv_metadata_header();
	if (summary_section) {
		machine_get_serialized_fields(FIELD_SCOPE_SYSTEM, system_fields,
					      &system_count);
		printf("Scope");
		for (int i = 0; i < system_count; i++) {
			putchar(',');
			print_csv_cell(system_fields[i]->json_label);
		}
	} else if (package_section) {
		machine_get_serialized_fields(FIELD_SCOPE_PACKAGE, package_fields,
					      &package_count);
		printf("Package");
		for (int i = 0; i < package_count; i++) {
			putchar(',');
			print_csv_cell(package_fields[i]->json_label);
		}
	} else if (cpu_section && section_emit_cpu_identity()) {
		machine_get_serialized_fields(FIELD_SCOPE_CPU, cpu_fields,
					      &cpu_count);
		printf("CPU");
		for (int i = 0; i < cpu_count; i++) {
			putchar(',');
			print_csv_cell(cpu_fields[i]->json_label);
		}
	}

	/* PMU data is defined at summary and CPU scope, not package scope. */
	if (is_pmu_enabled() && !package_section)
		print_pmu_csv_headers();
	printf("\n");
}

static void serialize_csv_cpu_row(const struct interval_record *rec,
				  int row_idx)
{
	struct field_desc *fields[MACHINE_FIELD_CAPACITY];
	int count;
	int first = 1;

	print_csv_metadata_prefix(rec);
	if (section_emit_cpu_identity()) {
		printf("%d", get_cpu_row_id(rec, row_idx));
		first = 0;
	}

	machine_get_serialized_fields(FIELD_SCOPE_CPU, fields, &count);
	for (int i = 0; i < count; i++) {
		char value[64];

		format_field_value(fields[i], rec, row_idx, "", value,
				   sizeof(value));
		printf("%s", first ? "" : ",");
		print_csv_cell(value);
		first = 0;
	}

	if (is_pmu_enabled())
		print_pmu_csv_cpu_values(rec, row_idx);
	printf("\n");
}

static void serialize_csv_package_row(const struct interval_record *rec,
				      int package_idx)
{
	struct field_desc *fields[MACHINE_FIELD_CAPACITY];
	int count;

	machine_get_serialized_fields(FIELD_SCOPE_PACKAGE, fields, &count);
	print_csv_metadata_prefix(rec);
	printf("%d", rec->packages[package_idx].package_id);
	for (int i = 0; i < count; i++) {
		char value[64];

		format_field_value(fields[i], rec, package_idx, "", value,
				   sizeof(value));
		putchar(',');
		print_csv_cell(value);
	}
	printf("\n");
}

static void serialize_csv_summary_row(const struct interval_record *rec)
{
	struct field_desc *fields[MACHINE_FIELD_CAPACITY];
	int count;

	machine_get_serialized_fields(FIELD_SCOPE_SYSTEM, fields, &count);
	print_csv_metadata_prefix(rec);
	printf("SUM");
	for (int i = 0; i < count; i++) {
		char value[64];

		format_field_value(fields[i], rec, 0, "", value, sizeof(value));
		putchar(',');
		print_csv_cell(value);
	}
	if (is_pmu_enabled())
		print_pmu_csv_summary_values(rec);
	printf("\n");
}

static void serialize_csv_mixed_cpu_row(const struct interval_record *rec,
					int row_idx, int system_count,
					int package_count,
					int summary_section)
{
	struct field_desc *cpu_fields[MACHINE_FIELD_CAPACITY];
	int cpu_count;

	print_csv_metadata_prefix(rec);
	printf("CPU,%d,", get_cpu_row_id(rec, row_idx));
	print_empty_csv_cells(system_count);
	print_empty_csv_cells(package_count);

	machine_get_serialized_fields(FIELD_SCOPE_CPU, cpu_fields, &cpu_count);
	for (int i = 0; i < cpu_count; i++) {
		char value[64];

		format_field_value(cpu_fields[i], rec, row_idx, "", value,
				   sizeof(value));
		putchar(',');
		print_csv_cell(value);
	}
	if (is_pmu_enabled() && summary_section)
		print_empty_csv_cells(rec->pmu_event_count);
	if (is_pmu_enabled())
		print_pmu_csv_cpu_values(rec, row_idx);
	printf("\n");
}

static void serialize_csv_mixed_package_row(const struct interval_record *rec,
					    int package_idx,
					    int system_count,
					    int cpu_count,
					    int summary_section,
					    int cpu_section)
{
	struct field_desc *package_fields[MACHINE_FIELD_CAPACITY];
	int package_count;

	machine_get_serialized_fields(FIELD_SCOPE_PACKAGE, package_fields,
				      &package_count);
	print_csv_metadata_prefix(rec);
	printf("PKG,,%d", rec->packages[package_idx].package_id);
	print_empty_csv_cells(system_count);
	for (int i = 0; i < package_count; i++) {
		char value[64];

		format_field_value(package_fields[i], rec, package_idx, "", value,
				   sizeof(value));
		putchar(',');
		print_csv_cell(value);
	}
	print_empty_csv_cells(cpu_count);
	if (is_pmu_enabled() && summary_section)
		print_empty_csv_cells(rec->pmu_event_count);
	if (is_pmu_enabled() && cpu_section)
		print_empty_csv_cells(rec->pmu_event_count);
	printf("\n");
}

static void serialize_csv_mixed_summary_row(const struct interval_record *rec,
					    int package_count,
					    int cpu_count,
					    int cpu_section)
{
	struct field_desc *system_fields[MACHINE_FIELD_CAPACITY];
	int system_count;

	machine_get_serialized_fields(FIELD_SCOPE_SYSTEM, system_fields,
				      &system_count);
	print_csv_metadata_prefix(rec);
	printf("SUM,,");
	for (int i = 0; i < system_count; i++) {
		char value[64];

		format_field_value(system_fields[i], rec, 0, "", value,
				   sizeof(value));
		putchar(',');
		print_csv_cell(value);
	}
	print_empty_csv_cells(package_count);
	print_empty_csv_cells(cpu_count);
	if (is_pmu_enabled())
		print_pmu_csv_summary_values(rec);
	if (is_pmu_enabled() && cpu_section)
		print_empty_csv_cells(rec->pmu_event_count);
	printf("\n");
}

static void serialize_csv_mixed_rows(const struct interval_record *rec,
				     int summary_section,
				     int package_section,
				     int cpu_section)
{
	struct field_desc *fields[MACHINE_FIELD_CAPACITY];
	int system_count = 0;
	int package_count = 0;
	int cpu_count = 0;

	if (summary_section)
		machine_get_serialized_fields(FIELD_SCOPE_SYSTEM, fields,
					      &system_count);
	if (package_section)
		machine_get_serialized_fields(FIELD_SCOPE_PACKAGE, fields,
					      &package_count);
	if (cpu_section)
		machine_get_serialized_fields(FIELD_SCOPE_CPU, fields, &cpu_count);

	if (summary_section)
		serialize_csv_mixed_summary_row(rec, package_count, cpu_count,
						cpu_section);
	if (package_section) {
		for (int i = 0; i < rec->package_count; i++)
			serialize_csv_mixed_package_row(rec, i, system_count,
							cpu_count, summary_section,
							cpu_section);
	}
	if (cpu_section) {
		for (int i = 0; i < rec->cpu_row_count; i++)
			serialize_csv_mixed_cpu_row(rec, i, system_count,
						     package_count,
						     summary_section);
	}
}

int serialize_csv(const struct interval_record *rec)
{
	struct csv_layout layout;
	int summary_section = section_is_summary_mode() ||
		section_emit_default_summary();
	int package_section = !section_is_summary_mode() &&
		section_emit_package();
	int cpu_section = !section_is_summary_mode() && section_emit_cpu();

	get_csv_layout(&layout);
	if (csv_header_printed &&
	    memcmp(&layout, &initial_csv_layout, sizeof(layout))) {
		fprintf(stderr,
			"Error: CSV columns changed during sampling; stopped before "
			"writing a mismatched row.\n"
			"  Restart capture for the new capabilities, or use JSON "
			"for changing column sets.\n");
		return -1;
	}
	if (!csv_header_printed) {
		initial_csv_layout = layout;
		serialize_csv_header();
		csv_header_printed = 1;
	}

	if (section_emit_mixed_csv()) {
		serialize_csv_mixed_rows(rec, summary_section, package_section,
					 cpu_section);
	} else if (summary_section) {
		serialize_csv_summary_row(rec);
	} else if (package_section) {
		for (int i = 0; i < rec->package_count; i++)
			serialize_csv_package_row(rec, i);
	} else if (cpu_section) {
		for (int i = 0; i < rec->cpu_row_count; i++)
			serialize_csv_cpu_row(rec, i);
	}

	fflush(stdout);
	return 0;
}

void reset_csv_state(void)
{
	csv_header_printed = 0;
}
