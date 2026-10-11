/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_json.c - JSON array and newline-delimited JSON serialization
 */

#include <math.h>
#include <stdio.h>

#include "formatter_machine.h"
#include "formatter_section.h"
#include "pmu.h"

static int json_first_interval = 1;

static const char *json_layout(int compact, const char *pretty)
{
	return compact ? "" : pretty;
}

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
				       int row_idx, int *needs_comma, int compact)
{
	if (*needs_comma)
		printf(",%s", json_layout(compact, "\n"));
	fputs(json_layout(compact, "      "), stdout);
	print_json_escaped_string(field->json_label);
	printf(": ");
	print_json_field_value(field, rec, row_idx);
	*needs_comma = 1;
}

static void print_pmu_json_summary_object(const struct interval_record *rec,
					  int compact)
{
	int pmu_count = rec->pmu_event_count;

	if (pmu_count <= 0 || !pmu_is_active() || !rec->summary.pmu_valid) {
		printf("null");
		return;
	}

	printf("{%s", json_layout(compact, "\n"));
	for (int i = 0; i < pmu_count; i++) {
		const char *name = get_pmu_event_name(i);

		fputs(json_layout(compact, "        "), stdout);
		print_json_escaped_string(name ? name : "event");
		printf(": %llu%s%s", rec->summary.pmu[i],
		       i < pmu_count - 1 ? "," : "",
		       json_layout(compact, "\n"));
	}
	printf("%s}", json_layout(compact, "      "));
}

static void print_pmu_json_cpu_object(const struct interval_record *rec,
				      int row_idx)
{
	int pmu_count = rec->pmu_event_count;

	if (pmu_count <= 0 || !pmu_is_active() ||
	    !rec->cpu_rows[row_idx].pmu_valid) {
		printf("null");
		return;
	}

	printf("{");
	for (int i = 0; i < pmu_count; i++) {
		const char *name = get_pmu_event_name(i);

		printf("%s", i ? ", " : "");
		print_json_escaped_string(name ? name : "event");
		printf(": %llu", rec->cpu_rows[row_idx].pmu[i]);
	}
	printf("}");
}

static void print_json_packages(const struct interval_record *rec,
				struct field_desc **fields, int field_count,
				int has_following_section, int compact)
{
	printf("%s\"packages\": [%s", json_layout(compact, "    "),
	       json_layout(compact, "\n"));
	for (int pkg = 0; pkg < rec->package_count; pkg++) {
		printf("%s{\"package\": %d", json_layout(compact, "      "),
		       rec->packages[pkg].package_id);
		for (int i = 0; i < field_count; i++)
			print_json_inline_field(fields[i], rec, pkg);
		printf("}%s%s", pkg < rec->package_count - 1 ? "," : "",
		       json_layout(compact, "\n"));
	}
	printf("%s]%s%s", json_layout(compact, "    "),
	       has_following_section ? "," : "", json_layout(compact, "\n"));
}

static void print_json_cpus(const struct interval_record *rec,
			    struct field_desc **fields, int field_count,
			    int has_summary, int compact)
{
	printf("%s\"cpus\": [%s", json_layout(compact, "    "),
	       json_layout(compact, "\n"));
	for (int row_idx = 0; row_idx < rec->cpu_row_count; row_idx++) {
		printf("%s{\"cpu\": %d", json_layout(compact, "      "),
		       get_cpu_row_id(rec, row_idx));
		for (int i = 0; i < field_count; i++)
			print_json_inline_field(fields[i], rec, row_idx);
		if (is_pmu_enabled()) {
			printf(", \"pmu\": ");
			print_pmu_json_cpu_object(rec, row_idx);
		}
		printf("}%s%s", row_idx < rec->cpu_row_count - 1 ? "," : "",
		       json_layout(compact, "\n"));
	}
	printf("%s]%s%s", json_layout(compact, "    "),
	       has_summary ? "," : "", json_layout(compact, "\n"));
}

static void print_json_summary(const struct interval_record *rec,
			       struct field_desc **fields, int field_count,
			       int compact)
{
	int needs_comma = 0;

	printf("%s\"summary\": {%s", json_layout(compact, "    "),
	       json_layout(compact, "\n"));
	for (int i = 0; i < field_count; i++)
		print_json_multiline_field(fields[i], rec, 0, &needs_comma,
					   compact);

	if (is_pmu_enabled()) {
		if (needs_comma)
			printf(",%s", json_layout(compact, "\n"));
		printf("%s\"pmu\": ", json_layout(compact, "      "));
		print_pmu_json_summary_object(rec, compact);
		needs_comma = 1;
	}

	if (needs_comma)
		fputs(json_layout(compact, "\n"), stdout);
	printf("%s}%s", json_layout(compact, "    "),
	       json_layout(compact, "\n"));
}

static void serialize_json_sample(const struct interval_record *rec, int compact)
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

	printf("%s{%s", json_layout(compact, "  "), json_layout(compact, "\n"));
	printf("%s\"schema_version\": %d,%s", json_layout(compact, "    "),
	       MACHINE_SCHEMA_VERSION, json_layout(compact, "\n"));
	printf("%s\"interval\": %llu,%s", json_layout(compact, "    "),
	       rec->interval, json_layout(compact, "\n"));
	printf("%s\"duration_us\": %llu,%s", json_layout(compact, "    "),
	       rec->duration_us, json_layout(compact, "\n"));
	printf("%s\"timestamp\": %ld,%s", json_layout(compact, "    "),
	       rec->timestamp, json_layout(compact, "\n"));
	printf("%s\"timestamp_ns\": %llu,%s", json_layout(compact, "    "),
	       machine_record_timestamp_ns(rec), json_layout(compact, "\n"));
	machine_format_timestamp_iso(rec, timestamp_iso, sizeof(timestamp_iso));
	printf("%s\"timestamp_iso\": ", json_layout(compact, "    "));
	print_json_escaped_string(timestamp_iso);
	printf("%s%s", emit_package || emit_cpu || emit_summary ? "," : "",
	       json_layout(compact, "\n"));

	if (emit_package)
		print_json_packages(rec, package_fields, package_field_count,
				    emit_cpu || emit_summary, compact);
	if (emit_cpu)
		print_json_cpus(rec, cpu_fields, cpu_field_count, emit_summary,
				compact);
	if (emit_summary)
		print_json_summary(rec, system_fields, system_field_count, compact);

	printf("%s}", json_layout(compact, "  "));
}

void serialize_json(const struct interval_record *rec)
{
	if (json_first_interval) {
		printf("[\n");
		json_first_interval = 0;
	} else {
		printf(",\n");
	}
	serialize_json_sample(rec, 0);
}

void serialize_ndjson(const struct interval_record *rec)
{
	serialize_json_sample(rec, 1);
	putchar('\n');
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
