/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_text.c - Stage 2: Text serialization
 *
 * Outputs human-readable text format using the field table.
 * Iterates through enabled fields instead of manual branching.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "formatter.h"
#include "formatter_machine.h"
#include "formatter_section.h"
#include "aggregator.h"
#include "pmu.h"
#include "topology.h"
#include "cpu_inventory.h"

/* Text formatter state */
static int quiet_mode = 0;
static int header_interval = 0;

#define TEXT_COLUMN_GAP 2
#define TEXT_ROW_KEY_WIDTH 5
#define TEXT_MAX_SYSTEM_FIELDS 32
#define TEXT_MAX_CPU_FIELDS 64
#define TEXT_VALUE_BUF_LEN 128
#define TEXT_TIMESTAMP_WIDTH 35

struct text_layout {
	struct field_desc *fields[TEXT_MAX_CPU_FIELDS];
	char labels[TEXT_MAX_CPU_FIELDS][TEXT_VALUE_BUF_LEN];
	int widths[TEXT_MAX_CPU_FIELDS];
	int count;
	int row_key_width;
	int pmu_widths[MAX_PMU_EVENTS];
	char pmu_labels[MAX_PMU_EVENTS][TEXT_VALUE_BUF_LEN];
	int pmu_count;
};

static struct text_layout previous_layouts[3];
static int previous_layout_valid;

static void format_text_label(const struct field_desc *field,
			      char *buf, size_t size)
{
	const char *label = field->label ? field->label : "";
	const char *unit = field->unit;

	if (!unit || !*unit || !strcmp(unit, "count") ||
	    (!strcmp(unit, "%") && strchr(label, '%'))) {
		snprintf(buf, size, "%s", label);
		return;
	}
	if (!strcmp(unit, "count/interval"))
		unit = "/int";
	else if (!strcmp(unit, "instructions/cycle"))
		unit = "insn/cyc";
	snprintf(buf, size, "%s[%s]", label, unit);
}

/* ============================================================================
 * SECTION 1: TEXT OUTPUT HELPERS
 * ============================================================================ */

static int get_default_field_width(const struct field_desc *field)
{
	int width = 0;

	switch (field->type) {
	case FIELD_TYPE_DOUBLE:
		width = 8;
		break;
	case FIELD_TYPE_LLONG:
		width = 10;
		break;
	case FIELD_TYPE_INT:
		width = 6;
		break;
	case FIELD_TYPE_STRING:
		width = 10;
		break;
	case FIELD_TYPE_BOOL:
		width = 5;
		break;
	}

	if (field->id && strcmp(field->id, "governor") == 0 && width < 11)
		width = 11;
	if (field->id && strcmp(field->id, "boost") == 0 && width < 5)
		width = 5;

	return width;
}

static int measure_text_field_width(const struct field_desc *field,
				    const struct interval_record *rec,
				    int row_idx)
{
	char buf[TEXT_VALUE_BUF_LEN];
	int width = get_default_field_width(field);

	if (field->label) {
		int label_len = (int)strlen(field->label);
		if (label_len > width)
			width = label_len;
	}

	format_field_value(field, rec, row_idx, "-", buf, sizeof(buf));
	{
		int value_len = (int)strlen(buf);
		if (value_len > width)
			width = value_len;
	}

	return width;
}

static int measure_text_pmu_width(const char *name, unsigned long long value, int available)
{
	char buf[TEXT_VALUE_BUF_LEN];
	int width = 0;

	snprintf(buf, sizeof(buf), "%s", available ? "" : "-");
	if (available)
		snprintf(buf, sizeof(buf), "%llu", value);

	width = (int)strlen(buf);
	if (name) {
		int name_len = (int)strlen(name);
		if (name_len > width)
			width = name_len;
	}

	if (width < 8)
		width = 8;

	return width;
}

static void build_text_layout(struct text_layout *layout,
			      enum field_scope scope,
			      const struct interval_record *rec)
{
	if (!layout)
		return;

	memset(layout, 0, sizeof(*layout));
	layout->row_key_width = TEXT_ROW_KEY_WIDTH;
	get_enabled_fields(scope, layout->fields, &layout->count);

	/* The package row key already renders the scope identity as "Pkg<N>". */
	if (scope == FIELD_SCOPE_PACKAGE) {
		for (int pkg = 0; pkg < rec->package_count; pkg++) {
			char key[16];
			int width;

			width = snprintf(key, sizeof(key), "Pkg%d",
					 rec->packages[pkg].package_id);
			if (width > layout->row_key_width)
				layout->row_key_width = width;
		}
		for (int i = 0; i < layout->count; i++) {
			if (field_is_scope_identity(layout->fields[i])) {
				for (int j = i; j < layout->count - 1; j++)
					layout->fields[j] = layout->fields[j + 1];
				layout->count--;
				break;
			}
		}
	}

	for (int i = 0; i < layout->count; i++) {
		int width = 0;

		if (scope == FIELD_SCOPE_SYSTEM) {
			width = measure_text_field_width(layout->fields[i], rec, 0);
		} else if (scope == FIELD_SCOPE_PACKAGE) {
			width = get_default_field_width(layout->fields[i]);
			if (layout->fields[i]->label) {
				int label_len = (int)strlen(layout->fields[i]->label);
				if (label_len > width)
					width = label_len;
			}
			for (int pkg = 0; pkg < rec->package_count; pkg++) {
				int c = measure_text_field_width(layout->fields[i], rec, pkg);
				if (c > width)
					width = c;
			}
		} else {
			width = get_default_field_width(layout->fields[i]);
			if (layout->fields[i]->label) {
				int label_len = (int)strlen(layout->fields[i]->label);
				if (label_len > width)
					width = label_len;
			}

			for (int row_idx = 0; row_idx < rec->cpu_row_count; row_idx++) {
				int candidate = measure_text_field_width(layout->fields[i],
								 rec, row_idx);
				if (candidate > width)
					width = candidate;
			}
		}

		format_text_label(layout->fields[i], layout->labels[i],
				  sizeof(layout->labels[i]));
		if ((int)strlen(layout->labels[i]) > width)
			width = (int)strlen(layout->labels[i]);
		layout->widths[i] = width;
	}

	if (!is_pmu_enabled())
		return;

	layout->pmu_count = get_pmu_event_count();
	for (int i = 0; i < layout->pmu_count; i++) {
		const char *name = get_pmu_event_name(i);

		snprintf(layout->pmu_labels[i], sizeof(layout->pmu_labels[i]),
			 "%s[/int]", name ? name : "event");
		name = layout->pmu_labels[i];

		if (scope == FIELD_SCOPE_SYSTEM) {
			unsigned long long value = rec ? rec->summary.pmu[i] : 0;
			int available = rec && pmu_is_active() &&
				rec->summary.pmu_valid;
			layout->pmu_widths[i] = measure_text_pmu_width(name, value, available);
		} else {
			int width = name ? (int)strlen(name) : 0;

			if (width < 8)
				width = 8;

			for (int row_idx = 0; row_idx < rec->cpu_row_count; row_idx++) {
				unsigned long long value = 0;
				int available = rec && pmu_is_active();
				int candidate;

				if (available)
					available = rec->cpu_rows[row_idx].pmu_valid;
				if (available)
					value = rec->cpu_rows[row_idx].pmu[i];
				candidate = measure_text_pmu_width(name, value, available);
				if (candidate > width)
					width = candidate;
			}

			layout->pmu_widths[i] = width;
		}
	}
}

static void print_text_separator(void)
{
	printf("%*s", TEXT_COLUMN_GAP, "");
}

static void print_text_field_with_width(const struct field_desc *field,
					const struct interval_record *rec,
					int row_idx,
					int width)
{
	char buf[TEXT_VALUE_BUF_LEN];

	format_field_value(field, rec, row_idx, "-", buf, sizeof(buf));
	printf("%*s", width, buf);
}

static void print_text_pmu_headers(const struct text_layout *layout)
{
	int count = layout ? layout->pmu_count : 0;

	for (int i = 0; i < count; i++) {
		print_text_separator();
		printf("%*s", layout->pmu_widths[i], layout->pmu_labels[i]);
	}
}

static void print_text_summary_pmu(const struct interval_record *rec,
				   const struct text_layout *layout)
{
	for (int i = 0; i < rec->pmu_event_count; i++) {
		print_text_separator();
		if (!pmu_is_active() || !rec->summary.pmu_valid)
			printf("%*s", layout->pmu_widths[i], "-");
		else
			printf("%*llu", layout->pmu_widths[i], rec->summary.pmu[i]);
	}
}

static void print_text_cpu_pmu(const struct interval_record *rec,
			       int row_idx,
			       const struct text_layout *layout)
{
	for (int i = 0; i < rec->pmu_event_count; i++) {
		print_text_separator();
		if (!pmu_is_active() || !rec->cpu_rows[row_idx].pmu_valid)
			printf("%*s", layout->pmu_widths[i], "-");
		else
			printf("%*llu", layout->pmu_widths[i],
			       rec->cpu_rows[row_idx].pmu[i]);
	}
}

/* ============================================================================
 * SECTION 2: TEXT HEADER
 * ============================================================================ */

static void serialize_text_package_header(const struct text_layout *layout)
{
	printf("%-*s", layout->row_key_width, "Pkg");
	for (int i = 0; i < layout->count; i++) {
		printf("%*s", TEXT_COLUMN_GAP, "");
		printf("%-*s", layout->widths[i], layout->labels[i]);
	}
	printf("\n");
}

static void serialize_text_package_row(const struct interval_record *rec,
					   int pkg_idx,
					   const struct text_layout *layout)
{
	char key[16];
	int package_id = rec->packages[pkg_idx].package_id;

	snprintf(key, sizeof(key), "Pkg%d", package_id);
	printf("%-*s", layout->row_key_width, key);
	for (int i = 0; i < layout->count; i++) {
		printf("%*s", TEXT_COLUMN_GAP, "");
		print_text_field_with_width(layout->fields[i], rec, pkg_idx,
					    layout->widths[i]);
	}
	printf("\n");
}

static void serialize_text_summary_header(const struct text_layout *layout)
{
	if (!any_fields_enabled(FIELD_SCOPE_SYSTEM))
		return;

	if (!quiet_mode && section_is_summary_mode())
		printf("%-*s  ", TEXT_TIMESTAMP_WIDTH, "Timestamp");
	printf("%-*s", TEXT_ROW_KEY_WIDTH, "SUM");
	for (int i = 0; i < layout->count; i++) {
		print_text_separator();
		printf("%*s", layout->widths[i], layout->labels[i]);
	}

	if (is_pmu_enabled())
		print_text_pmu_headers(layout);
	printf("\n");
}

static void serialize_text_cpu_header(const struct text_layout *layout)
{
	if (!any_fields_enabled(FIELD_SCOPE_CPU) &&
	    !show_cpu && !is_pmu_enabled())
			return;

	printf("%-*s", TEXT_ROW_KEY_WIDTH, section_emit_cpu_identity() ? "CPU" : "");
	for (int i = 0; i < layout->count; i++) {
		print_text_separator();
		printf("%*s", layout->widths[i], layout->labels[i]);
	}

	if (is_pmu_enabled())
		print_text_pmu_headers(layout);
	printf("\n");
}

/* ============================================================================
 * SECTION 3: TEXT SUMMARY ROW
 * ============================================================================ */

static void serialize_text_summary_row(const struct interval_record *rec,
				       const struct text_layout *layout)
{
	if (!quiet_mode && section_is_summary_mode()) {
		char timestamp[64];

		machine_format_timestamp_iso(rec, timestamp, sizeof(timestamp));
		printf("%-*s  ", TEXT_TIMESTAMP_WIDTH,
		       timestamp[0] ? timestamp : "-");
	}
	printf("%-*s", TEXT_ROW_KEY_WIDTH, "SUM");

	for (int i = 0; i < layout->count; i++) {
		print_text_separator();
		print_text_field_with_width(layout->fields[i], rec, 0,
					    layout->widths[i]);
	}

	if (is_pmu_enabled())
		print_text_summary_pmu(rec, layout);
	printf("\n");
}

/* ============================================================================
 * SECTION 4: TEXT CPU ROW
 * ============================================================================ */

static void serialize_text_cpu_row(const struct interval_record *rec,
				   int row_idx,
				   const struct text_layout *layout)
{
	int cpu_id = get_cpu_row_id(rec, row_idx);

	/* CPU column */
	if (section_emit_cpu_identity())
		printf("%-*d", TEXT_ROW_KEY_WIDTH, cpu_id);
	else
		printf("%-*s", TEXT_ROW_KEY_WIDTH, "");

	/* Per-CPU fields */
	for (int i = 0; i < layout->count; i++) {
		print_text_separator();
		print_text_field_with_width(layout->fields[i], rec, row_idx,
					    layout->widths[i]);
	}

	if (is_pmu_enabled())
		print_text_cpu_pmu(rec, row_idx, layout);

	printf("\n");
}

/* ============================================================================
 * SECTION 5: TEXT SERIALIZER API
 * ============================================================================ */

void serialize_text(const struct interval_record *rec)
{
	unsigned long long iteration = rec->interval;
	int print_header;
	int emit_summary;
	int emit_package;
	int emit_cpu;
	int section_count;
	int section_index;
	struct text_layout summary_layout;
	struct text_layout package_layout;
	struct text_layout cpu_layout;

	memset(&summary_layout, 0, sizeof(summary_layout));
	memset(&package_layout, 0, sizeof(package_layout));
	memset(&cpu_layout, 0, sizeof(cpu_layout));

	if (any_fields_enabled(FIELD_SCOPE_SYSTEM))
		build_text_layout(&summary_layout, FIELD_SCOPE_SYSTEM, rec);
	if (section_emit_package())
		build_text_layout(&package_layout, FIELD_SCOPE_PACKAGE, rec);
	if (section_emit_cpu())
		build_text_layout(&cpu_layout, FIELD_SCOPE_CPU, rec);

	print_header = (!quiet_mode && iteration == 1);
	if (!quiet_mode && header_interval > 0 &&
	    iteration > 1 && (iteration - 1) % header_interval == 0)
		print_header = 1;
	if (!quiet_mode && (!previous_layout_valid ||
	    memcmp(&summary_layout, &previous_layouts[0], sizeof(summary_layout)) ||
	    memcmp(&package_layout, &previous_layouts[1], sizeof(package_layout)) ||
	    memcmp(&cpu_layout, &previous_layouts[2], sizeof(cpu_layout))))
		print_header = 1;
	previous_layouts[0] = summary_layout;
	previous_layouts[1] = package_layout;
	previous_layouts[2] = cpu_layout;
	previous_layout_valid = 1;

	/*
	 * Default and expanded text views are multi-row sample blocks. Mark each
	 * block explicitly so the next interval cannot be mistaken for another
	 * section of the previous one. Summary-only mode remains one row per
	 * interval, and quiet mode remains compact for simple text pipelines.
	 */
	if (!quiet_mode && !section_is_summary_mode()) {
		char timestamp[64];

		machine_format_timestamp_iso(rec, timestamp, sizeof(timestamp));
		if (iteration > 1)
			putchar('\n');
		printf("--- interval %llu | %s | duration %.6f s ---\n",
		       iteration, timestamp[0] ? timestamp : "-",
		       rec->duration_us / 1000000.0);
	}

	if (section_is_summary_mode()) {
		/* Summary-only mode: one SUM block. */
		if (print_header)
			serialize_text_summary_header(&summary_layout);
		serialize_text_summary_row(rec, &summary_layout);
	} else {
		/*
		 * Mixed scope: keep each section self-contained so the SUM, Pkg,
		 * and CPU headers stay attached to their own rows instead of
		 * running together as one merged table. Sections are separated by
		 * a blank line.
		 */
		emit_summary = section_emit_default_summary();
		emit_package = section_emit_package();
		emit_cpu = section_emit_cpu();
		section_count = emit_summary + emit_package + emit_cpu;
		section_index = 0;

		if (emit_summary) {
			section_index++;
			if (print_header)
				serialize_text_summary_header(&summary_layout);
			serialize_text_summary_row(rec, &summary_layout);
			if (section_index < section_count)
				printf("\n");
		}

		if (emit_package) {
			section_index++;
			if (print_header)
				serialize_text_package_header(&package_layout);
			for (int pkg = 0; pkg < rec->package_count; pkg++)
				serialize_text_package_row(rec, pkg, &package_layout);
			if (section_index < section_count)
				printf("\n");
		}

		if (emit_cpu) {
			section_index++;
			if (print_header)
				serialize_text_cpu_header(&cpu_layout);
			for (int i = 0; i < rec->cpu_row_count; i++)
				serialize_text_cpu_row(rec, i, &cpu_layout);
		}
	}

	fflush(stdout);
}

/* ============================================================================
 * SECTION 6: TEXT CONFIG
 * ============================================================================ */

void set_text_quiet(int quiet)
{
	quiet_mode = quiet;
	previous_layout_valid = 0;
}

void set_text_header_interval(int interval)
{
	header_interval = interval;
}
