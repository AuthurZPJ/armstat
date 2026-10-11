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
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "formatter.h"
#include "formatter_machine.h"
#include "formatter_section.h"
#include "aggregator.h"
#include "pmu.h"
#include "topology.h"
#include "cpu_inventory.h"

/* Text formatter state; quiet_mode corresponds to --quiet-all. */
static int quiet_mode = 0;
static int header_interval = TEXT_HEADER_AUTO;
static int summary_screen_lines;
static int previous_terminal_rows;
static int previous_terminal_columns;

#define TEXT_COLUMN_GAP 2
#define TEXT_ROW_KEY_WIDTH 5
#define TEXT_MAX_SYSTEM_FIELDS 32
#define TEXT_MAX_CPU_FIELDS 64
#define TEXT_VALUE_BUF_LEN 128
#define TEXT_TIMESTAMP_WIDTH 29

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

struct text_terminal {
	int page_lines;
	int columns;
	int changed;
};

static struct text_layout previous_layouts[3];
static int previous_layout_valid;
static int previous_compact_summary;

/* Store only the rendered strings, retaining capacity between samples. */
struct text_values {
	size_t *offsets;
	size_t cell_capacity;
	char *data;
	size_t capacity;
	size_t used;
	int rows;
	int stride;
	int valid;
};

static struct text_values text_values[3];

static void prepare_text_values(enum field_scope scope, int rows, int stride)
{
	struct text_values *values = &text_values[scope];
	size_t cells = (size_t)rows * (size_t)stride;

	values->used = 0;
	values->rows = rows;
	values->stride = stride;
	values->valid = 1;
	if (cells > values->cell_capacity) {
		size_t *offsets = realloc(values->offsets, cells * sizeof(*offsets));

		if (!offsets) {
			values->valid = 0;
			return;
		}
		values->offsets = offsets;
		values->cell_capacity = cells;
	}
}

static void cache_text_value(enum field_scope scope, int row, int column,
			     const char *value)
{
	struct text_values *values = &text_values[scope];
	size_t length = strlen(value) + 1;
	size_t needed = values->used + length;

	if (!values->valid)
		return;
	if (needed > values->capacity) {
		size_t capacity = values->capacity ? values->capacity * 2 : 4096;
		char *data;

		if (capacity < needed)
			capacity = needed;
		data = realloc(values->data, capacity);
		if (!data) {
			values->valid = 0;
			return;
		}
		values->data = data;
		values->capacity = capacity;
	}
	values->offsets[(size_t)row * values->stride + column] = values->used;
	memcpy(values->data + values->used, value, length);
	values->used = needed;
}

static const char *cached_text_value(enum field_scope scope, int row, int column)
{
	const struct text_values *values = &text_values[scope];

	if (!values->valid || row < 0 || row >= values->rows ||
	    column < 0 || column >= values->stride)
		return NULL;
	return values->data + values->offsets[(size_t)row * values->stride + column];
}

static int read_text_terminal(struct text_terminal *terminal)
{
	struct winsize size;
	int fd = fileno(stdout);

	if (quiet_mode || header_interval != TEXT_HEADER_AUTO ||
	    !isatty(fd) || ioctl(fd, TIOCGWINSZ, &size) < 0 ||
	    !size.ws_row || !size.ws_col) {
		summary_screen_lines = 0;
		previous_terminal_rows = 0;
		previous_terminal_columns = 0;
		return 0;
	}

	/* Leave space for the interval marker and the terminal's current line. */
	terminal->page_lines = size.ws_row > 2 ? size.ws_row - 2 : 1;
	terminal->columns = size.ws_col;
	terminal->changed = previous_terminal_rows != size.ws_row ||
		previous_terminal_columns != size.ws_col;
	if (terminal->changed)
		summary_screen_lines = 0;
	previous_terminal_rows = size.ws_row;
	previous_terminal_columns = size.ws_col;
	return 1;
}

static int text_screen_lines(const struct text_layout *layout,
			     const struct text_terminal *terminal,
			     int compact_summary, int include_pmu)
{
	int width = layout->row_key_width;

	if (compact_summary)
		width += TEXT_TIMESTAMP_WIDTH + TEXT_COLUMN_GAP;
	for (int i = 0; i < layout->count; i++)
		width += TEXT_COLUMN_GAP + layout->widths[i];
	if (include_pmu)
		for (int i = 0; i < layout->pmu_count; i++)
			width += TEXT_COLUMN_GAP + layout->pmu_widths[i];

	return terminal->columns > 0 ? (width - 1) / terminal->columns + 1 : 1;
}

static int compact_summary_view(int emit_summary, int emit_package, int emit_cpu)
{
	return emit_summary && !emit_package && !emit_cpu;
}

static void format_text_timestamp(const struct interval_record *rec,
				  char *buf, size_t buf_size)
{
	char *fraction;

	machine_format_timestamp_iso(rec, buf, buf_size);
	/* Text needs milliseconds; retain the date and timezone verbatim. */
	fraction = strchr(buf, '.');
	if (fraction && strlen(fraction) >= 10)
		memmove(fraction + 4, fraction + 10, strlen(fraction + 10) + 1);
}

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
				    int row_idx, int column)
{
	char buf[TEXT_VALUE_BUF_LEN];
	int width = get_default_field_width(field);

	if (field->label) {
		int label_len = (int)strlen(field->label);
		if (label_len > width)
			width = label_len;
	}

	format_field_value(field, rec, row_idx, "-", buf, sizeof(buf));
	cache_text_value(field->scope, row_idx, column, buf);
	{
		int value_len = (int)strlen(buf);
		if (value_len > width)
			width = value_len;
	}

	return width;
}

static int measure_text_pmu_width(enum field_scope scope, int row, int column,
				 const char *name, unsigned long long value,
				 int available)
{
	char buf[TEXT_VALUE_BUF_LEN];
	int width = 0;

	snprintf(buf, sizeof(buf), "%s", available ? "" : "-");
	if (available)
		snprintf(buf, sizeof(buf), "%llu", value);
	cache_text_value(scope, row, column, buf);

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

	/* Package rows do not contain PMU cells. */
	layout->pmu_count = scope != FIELD_SCOPE_PACKAGE && is_pmu_enabled() ?
		get_pmu_event_count() : 0;
	prepare_text_values(scope, scope == FIELD_SCOPE_SYSTEM ? 1 :
			    scope == FIELD_SCOPE_PACKAGE ? rec->package_count :
			    rec->cpu_row_count, layout->count + layout->pmu_count);

	for (int i = 0; i < layout->count; i++) {
		int width = 0;

		if (scope == FIELD_SCOPE_SYSTEM) {
			width = measure_text_field_width(layout->fields[i], rec, 0, i);
		} else if (scope == FIELD_SCOPE_PACKAGE) {
			width = get_default_field_width(layout->fields[i]);
			if (layout->fields[i]->label) {
				int label_len = (int)strlen(layout->fields[i]->label);
				if (label_len > width)
					width = label_len;
			}
			for (int pkg = 0; pkg < rec->package_count; pkg++) {
				int c = measure_text_field_width(layout->fields[i], rec, pkg, i);
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
								 rec, row_idx, i);
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

	if (!layout->pmu_count)
		return;

	for (int i = 0; i < layout->pmu_count; i++) {
		const char *name = get_pmu_event_name(i);

		snprintf(layout->pmu_labels[i], sizeof(layout->pmu_labels[i]),
			 "%s[/int]", name ? name : "event");
		name = layout->pmu_labels[i];

		if (scope == FIELD_SCOPE_SYSTEM) {
			unsigned long long value = rec ? rec->summary.pmu[i] : 0;
			int available = rec && pmu_is_active() &&
				rec->summary.pmu_valid;
			layout->pmu_widths[i] = measure_text_pmu_width(scope, 0,
				layout->count + i, name, value, available);
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
				candidate = measure_text_pmu_width(scope, row_idx,
					layout->count + i, name, value, available);
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
					int column, int width)
{
	char buf[TEXT_VALUE_BUF_LEN];
	const char *value = cached_text_value(field->scope, row_idx, column);

	if (!value) {
		format_field_value(field, rec, row_idx, "-", buf, sizeof(buf));
		value = buf;
	}
	printf("%*s", width, value);
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
		const char *value = cached_text_value(FIELD_SCOPE_SYSTEM, 0,
						     layout->count + i);

		print_text_separator();
		if (value)
			printf("%*s", layout->pmu_widths[i], value);
		else if (!pmu_is_active() || !rec->summary.pmu_valid)
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
		const char *value = cached_text_value(FIELD_SCOPE_CPU, row_idx,
						     layout->count + i);

		print_text_separator();
		if (value)
			printf("%*s", layout->pmu_widths[i], value);
		else if (!pmu_is_active() || !rec->cpu_rows[row_idx].pmu_valid)
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
					    i, layout->widths[i]);
	}
	printf("\n");
}

static void serialize_text_summary_header(const struct text_layout *layout,
					 int compact_summary)
{
	if (!layout->count && !layout->pmu_count)
		return;

	if (!quiet_mode && compact_summary)
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
				       const struct text_layout *layout,
				       int compact_summary)
{
	if (!quiet_mode && compact_summary) {
		char timestamp[64];

		format_text_timestamp(rec, timestamp, sizeof(timestamp));
		printf("%-*s  ", TEXT_TIMESTAMP_WIDTH,
		       timestamp[0] ? timestamp : "-");
	}
	printf("%-*s", TEXT_ROW_KEY_WIDTH, "SUM");

	for (int i = 0; i < layout->count; i++) {
		print_text_separator();
		print_text_field_with_width(layout->fields[i], rec, 0,
					    i, layout->widths[i]);
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
					    i, layout->widths[i]);
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
	int compact_summary;
	int automatic_headers;
	int section_count;
	int section_index;
	struct text_terminal terminal = {0};
	struct text_layout summary_layout;
	struct text_layout package_layout;
	struct text_layout cpu_layout;

	memset(&summary_layout, 0, sizeof(summary_layout));
	memset(&package_layout, 0, sizeof(package_layout));
	memset(&cpu_layout, 0, sizeof(cpu_layout));

	emit_summary = section_is_summary_mode() || section_emit_default_summary();
	emit_package = !section_is_summary_mode() && section_emit_package();
	emit_cpu = !section_is_summary_mode() && section_emit_cpu();
	compact_summary = compact_summary_view(emit_summary, emit_package, emit_cpu);

	if (emit_summary)
		build_text_layout(&summary_layout, FIELD_SCOPE_SYSTEM, rec);
	if (emit_package)
		build_text_layout(&package_layout, FIELD_SCOPE_PACKAGE, rec);
	if (emit_cpu)
		build_text_layout(&cpu_layout, FIELD_SCOPE_CPU, rec);
	automatic_headers = read_text_terminal(&terminal);

	print_header = (!quiet_mode && iteration == 1);
	if (!quiet_mode && header_interval > 0 &&
	    iteration > 1 && (iteration - 1) % header_interval == 0)
		print_header = 1;
	if (!quiet_mode && (!previous_layout_valid ||
	    previous_compact_summary != compact_summary ||
	    memcmp(&summary_layout, &previous_layouts[0], sizeof(summary_layout)) ||
	    memcmp(&package_layout, &previous_layouts[1], sizeof(package_layout)) ||
	    memcmp(&cpu_layout, &previous_layouts[2], sizeof(cpu_layout))))
		print_header = 1;
	if (automatic_headers && terminal.changed)
		print_header = 1;
	previous_layouts[0] = summary_layout;
	previous_layouts[1] = package_layout;
	previous_layouts[2] = cpu_layout;
	previous_layout_valid = 1;
	previous_compact_summary = compact_summary;

	/*
	 * A single SUM uses a continuous table regardless of how it was selected.
	 * Other views retain interval blocks so multiple rows stay attributable
	 * to their sample. --quiet-all omits timestamps and block markers.
	 */
	if (!quiet_mode && !compact_summary) {
		char timestamp[64];

		format_text_timestamp(rec, timestamp, sizeof(timestamp));
		if (iteration > 1)
			putchar('\n');
		printf("--- interval %llu | %s | duration %.6f s ---\n",
		       iteration, timestamp[0] ? timestamp : "-",
		       rec->duration_us / 1000000.0);
	}

	if (compact_summary) {
		/* Single summary: one data row per interval. */
		int screen_lines = automatic_headers ?
			text_screen_lines(&summary_layout, &terminal, 1, 1) : 0;

		if (automatic_headers && screen_lines * 2 <= terminal.page_lines &&
		    (!summary_screen_lines ||
		    summary_screen_lines + screen_lines > terminal.page_lines))
			print_header = 1;
		if (print_header) {
			serialize_text_summary_header(&summary_layout, compact_summary);
			summary_screen_lines = screen_lines;
		}
		serialize_text_summary_row(rec, &summary_layout, compact_summary);
		if (automatic_headers && screen_lines * 2 <= terminal.page_lines)
			summary_screen_lines += screen_lines;
		else
			summary_screen_lines = 0;
	} else {
		/*
		 * Mixed scope: keep each section self-contained so the SUM, Pkg,
		 * and CPU headers stay attached to their own rows instead of
		 * running together as one merged table. Sections are separated by
		 * a blank line.
		 */
		section_count = emit_summary + emit_package + emit_cpu;
		section_index = 0;
		summary_screen_lines = 0;
		if (automatic_headers)
			print_header = 1;

		if (emit_summary) {
			section_index++;
			if (print_header)
				serialize_text_summary_header(&summary_layout,
							      compact_summary);
			serialize_text_summary_row(rec, &summary_layout, compact_summary);
			if (section_index < section_count)
				printf("\n");
		}

		if (emit_package) {
			int screen_lines = automatic_headers ?
				text_screen_lines(&package_layout, &terminal, 0, 0) : 0;
			int page_lines = screen_lines;

			section_index++;
			if (print_header)
				serialize_text_package_header(&package_layout);
			for (int pkg = 0; pkg < rec->package_count; pkg++) {
				if (automatic_headers && pkg > 0 &&
				    screen_lines * 2 <= terminal.page_lines &&
				    page_lines + screen_lines > terminal.page_lines) {
					serialize_text_package_header(&package_layout);
					page_lines = screen_lines;
				}
				serialize_text_package_row(rec, pkg, &package_layout);
				page_lines += screen_lines;
			}
			if (section_index < section_count)
				printf("\n");
		}

		if (emit_cpu) {
			int screen_lines = automatic_headers ?
				text_screen_lines(&cpu_layout, &terminal, 0, 1) : 0;
			int page_lines = screen_lines;

			section_index++;
			if (print_header)
				serialize_text_cpu_header(&cpu_layout);
			for (int i = 0; i < rec->cpu_row_count; i++) {
				if (automatic_headers && i > 0 &&
				    screen_lines * 2 <= terminal.page_lines &&
				    page_lines + screen_lines > terminal.page_lines) {
					serialize_text_cpu_header(&cpu_layout);
					page_lines = screen_lines;
				}
				serialize_text_cpu_row(rec, i, &cpu_layout);
				page_lines += screen_lines;
			}
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
	summary_screen_lines = 0;
	previous_terminal_rows = 0;
	previous_terminal_columns = 0;
}

void set_text_header_interval(int interval)
{
	header_interval = interval;
	summary_screen_lines = 0;
	previous_terminal_rows = 0;
	previous_terminal_columns = 0;
}

void cleanup_text_values(void)
{
	for (int i = 0; i < 3; i++) {
		free(text_values[i].offsets);
		free(text_values[i].data);
		memset(&text_values[i], 0, sizeof(text_values[i]));
	}
}
