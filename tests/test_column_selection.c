// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal regression tests for exact field selection/hiding semantics.
 *
 * These tests include armstat.c directly so they can exercise the same static
 * parse_column_option() helper used by the real CLI without exporting extra
 * production-only symbols.
 */

#include <assert.h>
#include <string.h>

#include "armstat_cli.h"
#include "formatter.h"
#include "formatter_section.h"

static int scope_has_field(enum field_scope scope, const char *json_label)
{
	struct field_desc *fields[64];
	int count = 0;

	get_enabled_fields(scope, fields, &count);
	for (int i = 0; i < count; i++) {
		if (fields[i]->json_label &&
		    strcmp(fields[i]->json_label, json_label) == 0)
			return 1;
	}

	return 0;
}

static struct field_desc *require_field(const char *field_id)
{
	struct field_desc *field = get_field_desc(field_id);

	assert(field != NULL);
	return field;
}

static void test_static_default_column_state_inherits_group_visibility(void)
{
	struct field_desc *power = require_field("power_mw");

	assert(scope_has_field(FIELD_SCOPE_SYSTEM, "avg_freq"));
	assert(scope_has_field(FIELD_SCOPE_CPU, "freq"));
	assert(scope_has_field(FIELD_SCOPE_CPU, "idle_percent"));
	assert(!scope_has_field(FIELD_SCOPE_SYSTEM, "uncore_freq"));
	assert(power->scope == FIELD_SCOPE_SYSTEM);
}

static void test_field_registry_keys_are_unique(void)
{
	struct field_desc *fields = get_all_fields();
	int count = get_field_count();

	for (int i = 0; i < count; i++) {
		assert(fields[i].id != NULL);
		assert(fields[i].json_label != NULL);
		for (int j = i + 1; j < count; j++) {
			assert(strcmp(fields[i].id, fields[j].id) != 0);
			if (fields[i].scope == fields[j].scope)
				assert(strcmp(fields[i].json_label,
					      fields[j].json_label) != 0);
		}
	}
}

static void test_hide_exact_summary_field_keeps_group_alive(void)
{
	reset_columns();
	parse_column_option("uncore_freq", 0);

	assert(show_freq == 1);
	assert(scope_has_field(FIELD_SCOPE_SYSTEM, "avg_freq"));
	assert(!scope_has_field(FIELD_SCOPE_SYSTEM, "uncore_freq"));
}

static void test_hide_exact_cpu_field_keeps_group_alive(void)
{
	reset_columns();
	parse_column_option("boost", 0);

	assert(show_freq == 1);
	assert(scope_has_field(FIELD_SCOPE_CPU, "freq"));
	assert(scope_has_field(FIELD_SCOPE_CPU, "governor"));
	assert(!scope_has_field(FIELD_SCOPE_CPU, "boost"));
}

static void test_show_exact_field_whitelists_only_that_field(void)
{
	clear_columns();
	parse_column_option("uncore_freq", 1);

	assert(show_freq == 1);
	assert(!scope_has_field(FIELD_SCOPE_SYSTEM, "avg_freq"));
	if (has_uncore_freq_support())
		assert(scope_has_field(FIELD_SCOPE_SYSTEM, "uncore_freq"));
	else
		assert(!scope_has_field(FIELD_SCOPE_SYSTEM, "uncore_freq"));
}

static void test_show_exact_package_field_enables_package_section(void)
{
	clear_columns();
	set_section_default_summary_output(1);
	parse_column_option("pkg_avg_freq", 1);

	assert(show_package == 1);
	assert(scope_has_field(FIELD_SCOPE_PACKAGE, "freq"));
	assert(!scope_has_field(FIELD_SCOPE_SYSTEM, "avg_freq"));
	assert(!scope_has_field(FIELD_SCOPE_CPU, "freq"));
	assert(section_emit_package() == 1);
}

static void test_show_idle_percent_does_not_reenable_split_lpi_fields(void)
{
	clear_columns();
	parse_column_option("Idle%", 1);

	assert(show_idle == 1);
	assert(scope_has_field(FIELD_SCOPE_SYSTEM, "idle_percent"));
	assert(scope_has_field(FIELD_SCOPE_CPU, "idle_percent"));
	assert(!scope_has_field(FIELD_SCOPE_SYSTEM, "lpi0"));
	assert(!scope_has_field(FIELD_SCOPE_CPU, "lpi0"));
}

static void test_idle_group_whitelist_obeys_runtime_lpi_availability(void)
{
	clear_columns();
	parse_column_option("idle", 1);

	assert(show_idle == 1);
	assert(show_iowait == 1);
	assert(scope_has_field(FIELD_SCOPE_SYSTEM, "idle_percent"));
	assert(scope_has_field(FIELD_SCOPE_CPU, "idle_percent"));
	assert(!scope_has_field(FIELD_SCOPE_SYSTEM, "lpi0"));
	assert(!scope_has_field(FIELD_SCOPE_CPU, "lpi0"));
}

static void test_temp_group_whitelist_obeys_runtime_temp_availability(void)
{
	clear_columns();
	parse_column_option("temp", 1);
	update_temp_field_visibility();

	assert(show_temp == 1);
	assert(!scope_has_field(FIELD_SCOPE_SYSTEM, "temp0"));
	assert(!scope_has_field(FIELD_SCOPE_SYSTEM, "temp1"));
	assert(!scope_has_field(FIELD_SCOPE_CPU, "temp"));
}

static void test_temp_and_idle_field_metadata(void)
{
	struct field_desc *summary_temp = require_field("temp_vdie0");
	struct field_desc *cpu_temp = require_field("cpu_temp_c");
	struct field_desc *cpu_lpi0 = require_field("idle_state0");
	struct field_desc *cpu_lpi0_wake = require_field("idle_state_wakeup0");

	assert((summary_temp->group_mask & FIELD_GROUP_TEMP) != 0);
	assert(summary_temp->series == FIELD_SERIES_SUMMARY_TEMP);
	assert(summary_temp->series_index == 0);
	assert(summary_temp->enabled_ptr == &show_temp);
	assert((cpu_temp->group_mask & FIELD_GROUP_TEMP) != 0);
	assert(cpu_temp->series == FIELD_SERIES_NONE);
	assert(cpu_lpi0->series == FIELD_SERIES_IDLE_STATE);
	assert(cpu_lpi0->series_index == 0);
	assert(cpu_lpi0_wake->series == FIELD_SERIES_IDLE_STATE);
	assert(cpu_lpi0_wake->series_index == 0);
	assert(strcmp(cpu_lpi0_wake->json_label, "lpi0_wake") == 0);
	assert(strcmp(cpu_lpi0->label, cpu_lpi0_wake->label) != 0);
}

static void test_all_columns_enable_base_groups_but_not_pmu_or_ipc(void)
{
	clear_columns();
	set_all_columns_enabled(1);

	assert(show_cpu == 1);
	assert(show_freq == 1);
	assert(show_idle == 1);
	assert(show_power == 1);
	assert(show_temp == 1);
	assert(show_sysstat == 1);
	assert(show_membw == 1);
	assert(show_package == 1);
	assert(show_core == 1);
	assert(show_numa == 1);
	assert(show_energy == 1);
	assert(show_pmu == 0);
	assert(show_ipc == 0);
}

static void test_show_all_remains_union_with_later_tokens(void)
{
	clear_columns();
	assert(parse_column_option("all,avg_mhz", 1) == 0);

	assert(scope_has_field(FIELD_SCOPE_SYSTEM, "avg_freq"));
	assert(scope_has_field(FIELD_SCOPE_SYSTEM, "power"));
	assert(scope_has_field(FIELD_SCOPE_SYSTEM, "ctx_switches"));
	assert(scope_has_field(FIELD_SCOPE_CPU, "freq"));

	clear_columns();
	assert(parse_column_option("all,freq", 1) == 0);

	assert(scope_has_field(FIELD_SCOPE_SYSTEM, "avg_freq"));
	assert(scope_has_field(FIELD_SCOPE_SYSTEM, "power"));
	assert(scope_has_field(FIELD_SCOPE_SYSTEM, "ctx_switches"));
	assert(scope_has_field(FIELD_SCOPE_CPU, "freq"));
}

static void test_hide_all_turns_off_explicit_pmu_and_ipc(void)
{
	clear_columns();
	enable_pmu(1);
	enable_ipc(1);

	parse_column_option("all", 0);

	assert(show_cpu == 0);
	assert(show_freq == 0);
	assert(show_idle == 0);
	assert(show_power == 0);
	assert(show_temp == 0);
	assert(show_sysstat == 0);
	assert(show_membw == 0);
	assert(show_package == 0);
	assert(show_core == 0);
	assert(show_numa == 0);
	assert(show_energy == 0);
	assert(show_pmu == 0);
	assert(show_ipc == 0);
}

int main(void)
{
	test_field_registry_keys_are_unique();
	test_static_default_column_state_inherits_group_visibility();
	test_hide_exact_summary_field_keeps_group_alive();
	test_hide_exact_cpu_field_keeps_group_alive();
	test_show_exact_field_whitelists_only_that_field();
	test_show_exact_package_field_enables_package_section();
	test_show_idle_percent_does_not_reenable_split_lpi_fields();
	test_idle_group_whitelist_obeys_runtime_lpi_availability();
	test_temp_group_whitelist_obeys_runtime_temp_availability();
	test_temp_and_idle_field_metadata();
	test_all_columns_enable_base_groups_but_not_pmu_or_ipc();
	test_show_all_remains_union_with_later_tokens();
	test_hide_all_turns_off_explicit_pmu_and_ipc();
	return 0;
}
