/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_json.c - JSON stream serialization
 */

#include <math.h>
#include <stdio.h>

#include "formatter_machine.h"
#include "formatter_section.h"
#include "pmu.h"

static int json_first_interval = 1;

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

static void print_json_field_value(const struct field_desc *field,
				   const struct interval_record *rec,
				   int row_idx)
{
	switch (field->type) {
	case FIELD_TYPE_DOUBLE: {
		double value = field->getter.get_double(rec, row_idx);

		if (!isfinite(value))
			printf("null");
		else
			printf("%.*f", field->decimals, value);
		break;
	}
	case FIELD_TYPE_LLONG:
		printf("%lld", field->getter.get_llong(rec, row_idx));
		break;
	case FIELD_TYPE_INT:
		printf("%d", field->getter.get_int(rec, row_idx));
		break;
	case FIELD_TYPE_STRING: {
		const char *value = field->getter.get_string(rec, row_idx);

		if (!value)
			printf("null");
		else
			print_json_escaped_string(value);
		break;
	}
	case FIELD_TYPE_BOOL: {
		int value = field->getter.get_int(rec, row_idx);

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
				    int row_idx)
{
	printf(", ");
	print_json_escaped_string(field->json_label);
	printf(": ");
	print_json_field_value(field, rec, row_idx);
}

static void print_json_multiline_field(const struct field_desc *field,
				       const struct interval_record *rec,
				       int row_idx, int *needs_comma)
{
	printf("%s      ", *needs_comma ? ",\n" : "");
	print_json_escaped_string(field->json_label);
	printf(": ");
	print_json_field_value(field, rec, row_idx);
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
		printf(": %llu%s\n", rec->summary.pmu[i],
		       i < pmu_count - 1 ? "," : "");
	}
	printf("      }");
}

static void print_pmu_json_cpu_object(const struct interval_record *rec,
				      int cpu_idx)
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

static void print_json_packages(const struct interval_record *rec,
				struct field_desc **fields, int field_count,
				int has_following_section)
{
	printf("    \"packages\": [\n");
	for (int pkg = 0; pkg < rec->package_count; pkg++) {
		printf("      {\"package\": %d", rec->packages[pkg].package_id);
		for (int i = 0; i < field_count; i++)
			print_json_inline_field(fields[i], rec, pkg);
		printf("}%s\n", pkg < rec->package_count - 1 ? "," : "");
	}
	printf("    ]%s\n", has_following_section ? "," : "");
}

static void print_json_cpus(const struct interval_record *rec,
			    struct field_desc **fields, int field_count,
			    int has_summary)
{
	printf("    \"cpus\": [\n");
	for (int row = 0; row < rec->cpu_row_count; row++) {
		int cpu_idx = rec->cpu_rows[row].cpu_idx;

		printf("      {\"cpu\": %d", get_cpu_row_id(rec, row));
		for (int i = 0; i < field_count; i++)
			print_json_inline_field(fields[i], rec, cpu_idx);
		if (is_pmu_enabled()) {
			printf(", \"pmu\": ");
			print_pmu_json_cpu_object(rec, cpu_idx);
		}
		printf("}%s\n", row < rec->cpu_row_count - 1 ? "," : "");
	}
	printf("    ]%s\n", has_summary ? "," : "");
}

static void print_json_summary(const struct interval_record *rec,
			       struct field_desc **fields, int field_count)
{
	int needs_comma = 0;

	printf("    \"summary\": {\n");
	for (int i = 0; i < field_count; i++)
		print_json_multiline_field(fields[i], rec, 0, &needs_comma);

	if (is_pmu_enabled()) {
		printf("%s      \"pmu\": ", needs_comma ? ",\n" : "");
		print_pmu_json_summary_object(rec);
		needs_comma = 1;
	}

	if (needs_comma)
		printf("\n");
	printf("    }\n");
}

void serialize_json(const struct interval_record *rec)
{
	struct field_desc *cpu_fields[MACHINE_FIELD_CAPACITY];
	struct field_desc *package_fields[MACHINE_FIELD_CAPACITY];
	struct field_desc *system_fields[MACHINE_FIELD_CAPACITY];
	int emit_cpu = !section_is_summary_mode() && section_emit_cpu();
	int emit_summary = section_is_summary_mode() ||
		section_emit_default_summary();
	int emit_package = !section_is_summary_mode() && section_emit_package() &&
		rec->package_count > 0;
	int cpu_field_count = 0;
	int package_field_count = 0;
	int system_field_count = 0;
	char timestamp_iso[64];

	if (emit_cpu)
		machine_get_serialized_fields(FIELD_SCOPE_CPU, cpu_fields,
					      &cpu_field_count);
	if (emit_package)
		machine_get_serialized_fields(FIELD_SCOPE_PACKAGE, package_fields,
					      &package_field_count);
	if (emit_summary)
		machine_get_serialized_fields(FIELD_SCOPE_SYSTEM, system_fields,
					      &system_field_count);

	if (json_first_interval) {
		printf("[\n");
		json_first_interval = 0;
	} else {
		printf(",\n");
	}

	printf("  {\n");
	printf("    \"schema_version\": %d,\n", MACHINE_SCHEMA_VERSION);
	printf("    \"interval\": %llu,\n", rec->interval);
	printf("    \"duration_us\": %llu,\n", rec->duration_us);
	printf("    \"timestamp\": %ld,\n", rec->timestamp);
	printf("    \"timestamp_ns\": %llu,\n",
	       machine_record_timestamp_ns(rec));
	machine_format_timestamp_iso(rec, timestamp_iso, sizeof(timestamp_iso));
	printf("    \"timestamp_iso\": ");
	print_json_escaped_string(timestamp_iso);
	printf("%s\n", emit_package || emit_cpu || emit_summary ? "," : "");

	if (emit_package)
		print_json_packages(rec, package_fields, package_field_count,
				    emit_cpu || emit_summary);
	if (emit_cpu)
		print_json_cpus(rec, cpu_fields, cpu_field_count, emit_summary);
	if (emit_summary)
		print_json_summary(rec, system_fields, system_field_count);

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

void reset_json_state(void)
{
	json_first_interval = 1;
}
