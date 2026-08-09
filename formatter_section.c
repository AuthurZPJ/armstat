/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_section.c - Section emission policy
 *
 * Owns the "which output section is emitted this interval" decisions shared
 * by the text, JSON, and CSV serializers, plus the summary-mode state those
 * decisions depend on.
 *
 * Consumers (formatter_text.c / formatter_machine.c) only ask "should I emit
 * section X?"; they no longer carry their own copies of the policy or the
 * summary-mode state. The CLI (armstat_cli.c) calls set_section_*() here, so
 * the state has exactly one home.
 */

#include "formatter_section.h"
#include "formatter.h"
#include "cpu_inventory.h"

/* Section emission state, configured by the CLI (-S / -a). */
static int summary_mode = 0;
static int default_summary_output = 0;

int section_emit_cpu(void)
{
	return show_cpu || show_pmu || any_fields_enabled(FIELD_SCOPE_CPU);
}

int section_emit_cpu_identity(void)
{
	/*
	 * Per-CPU rows are only useful when the row key is visible. Keep the
	 * CPU identity column for every per-CPU output, even if the user hid
	 * the "cpu" group, so rows remain interpretable.
	 */
	return section_emit_cpu();
}

int section_emit_package(void)
{
	/*
	 * Package rows are aggregation rows. They only appear when the package
	 * column group is explicitly requested (via -a or -s package); the
	 * default output is per-CPU rows only.
	 *
	 * Package rows aggregate across all CPUs in a package, which also
	 * conflicts with explicit CPU filtering. When --cpu is active, suppress
	 * package rows just like we suppress the automatic SUM section.
	 */
	if (cpu_inventory_filter_is_active())
		return 0;
	return show_package && any_fields_enabled(FIELD_SCOPE_PACKAGE);
}

int section_emit_default_summary(void)
{
	return default_summary_output &&
	       any_fields_enabled(FIELD_SCOPE_SYSTEM) &&
	       !cpu_inventory_filter_is_active();
}

int section_emit_mixed_csv(void)
{
	return !summary_mode &&
	       section_emit_cpu() &&
	       section_emit_default_summary();
}

int section_is_summary_mode(void)
{
	return summary_mode;
}

int section_default_summary_output(void)
{
	return default_summary_output;
}

void set_section_summary_mode(int summary)
{
	summary_mode = summary;
}

void set_section_default_summary_output(int enable)
{
	default_summary_output = enable;
}
