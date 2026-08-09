// SPDX-License-Identifier: GPL-2.0
/*
 * Unit tests for the section emission policy (formatter_section.c).
 *
 * Drives the policy directly with controlled field visibility, summary-mode
 * state, and CPU filter state, asserting which output sections are emitted.
 * These cover the decisions previously buried in each serializer:
 *   - default mode: per-CPU rows only
 *   - -a: SUM + package + CPU sections
 *   - -S: summary-only mode
 *   - --cpu filter: suppresses package and default-SUM aggregation
 *   - clear_columns: no sections
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../armstat_cli.h"
#include "../cpu_inventory.h"
#include "../formatter.h"
#include "../formatter_section.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static void reset_policy_state(void)
{
	reset_columns();
	clear_field_overrides();
	set_section_summary_mode(0);
	set_section_default_summary_output(0);
	set_cpu_inventory_filter(NULL);
}

static void test_default_mode(void)
{
	reset_policy_state();

	CHECK(section_is_summary_mode() == 0);
	CHECK(section_emit_cpu() == 1);
	CHECK(section_emit_cpu_identity() == 1);
	CHECK(section_emit_package() == 0);
	CHECK(section_emit_default_summary() == 0);
	CHECK(section_emit_mixed_csv() == 0);
}

static void test_all_columns_mode(void)
{
	reset_policy_state();
	set_all_columns_enabled(1);
	set_section_default_summary_output(1);

	CHECK(section_is_summary_mode() == 0);
	CHECK(section_emit_cpu() == 1);
	CHECK(section_emit_package() == 1);
	CHECK(section_emit_default_summary() == 1);
	CHECK(section_emit_mixed_csv() == 1);
}

static void test_summary_mode(void)
{
	reset_policy_state();
	set_section_summary_mode(1);

	CHECK(section_is_summary_mode() == 1);
	/* -S is summary-only; the default SUM section and mixed CSV stay off. */
	CHECK(section_emit_default_summary() == 0);
	CHECK(section_emit_mixed_csv() == 0);
}

static void test_cpu_filter_suppresses_aggregation(void)
{
	reset_policy_state();
	set_all_columns_enabled(1);
	set_section_default_summary_output(1);

	CHECK(section_emit_package() == 1);
	CHECK(section_emit_default_summary() == 1);

	CHECK(set_cpu_inventory_filter("0") == 0);
	CHECK(section_emit_package() == 0);
	CHECK(section_emit_default_summary() == 0);
	/* Per-CPU rows survive the filter. */
	CHECK(section_emit_cpu() == 1);
}

static void test_clear_columns(void)
{
	reset_policy_state();
	clear_columns();

	CHECK(section_emit_cpu() == 0);
	CHECK(section_emit_cpu_identity() == 0);
	CHECK(section_emit_package() == 0);
	CHECK(section_emit_default_summary() == 0);
	CHECK(section_emit_mixed_csv() == 0);
}

int main(void)
{
	test_default_mode();
	test_all_columns_mode();
	test_summary_mode();
	test_cpu_filter_suppresses_aggregation();
	test_clear_columns();

	if (failures) {
		printf("test_section_policy: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_section_policy: all tests passed\n");
	return 0;
}
