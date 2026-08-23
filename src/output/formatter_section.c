/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_section.c - Section emission policy
 *
 * Owns the "which output section is emitted this interval" decisions shared
 * by the text, JSON, and CSV serializers, plus the summary-mode state those
 * decisions depend on.
 *
 * Consumers (formatter_text.c / formatter_json.c / formatter_csv.c) only ask
 * "should I emit section X?"; they no longer carry their own copies of the
 * policy or the summary-mode state. The CLI (armstat_cli.c) calls
 * set_section_*() here, so the state has exactly one home.
 */

#include "formatter_section.h"
#include "formatter.h"
#include "cpu_inventory.h"

/* Section emission state, configured by the CLI (-S / -a). */
static int summary_mode = 0;
static int default_summary_output = 1;

int section_emit_cpu(void)
{
	/*
	 * CPU-scope fields share group visibility with summary/package fields, so
	 * field availability alone must not expand a many-core default view.
	 * show_cpu is the explicit row-level switch set by -a, -s cpu, or an exact
	 * CPU field request. PMU remains an implicit CPU-row request.
	 */
	return show_cpu || show_pmu;
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
	 * Package rows are aggregation rows. They are part of the concise default
	 * view and remain selectable explicitly via -a or -s package.
	 *
	 * With --cpu, avoid implicitly mixing filtered CPU rows with aggregate
	 * rows. An explicit package-only selection remains useful and is computed
	 * over the filtered tracked CPU set.
	 */
	if (cpu_inventory_filter_is_active() && section_emit_cpu())
		return 0;
	return show_package && any_fields_enabled(FIELD_SCOPE_PACKAGE);
}

int section_emit_default_summary(void)
{
	if (summary_mode || !default_summary_output ||
	    !any_fields_enabled(FIELD_SCOPE_SYSTEM))
		return 0;
	return !cpu_inventory_filter_is_active() || !section_emit_cpu();
}

int section_emit_mixed_csv(void)
{
	int section_count;

	if (summary_mode)
		return 0;

	section_count = section_emit_cpu() + section_emit_package() +
			section_emit_default_summary();
	return section_count > 1;
}

int section_is_summary_mode(void)
{
	return summary_mode;
}

int section_default_summary_output(void)
{
	return default_summary_output;
}

int section_has_output(void)
{
	if (summary_mode)
		return any_fields_enabled(FIELD_SCOPE_SYSTEM) || show_pmu;

	return section_emit_cpu() || section_emit_package() ||
	       section_emit_default_summary();
}

void section_get_idle_collection_masks(unsigned int *residency_mask,
				       unsigned int *usage_mask)
{
	unsigned int residency = 0;
	unsigned int usage = 0;

	if (summary_mode || section_emit_default_summary())
		residency |= get_enabled_series_mask(
			FIELD_SCOPE_SYSTEM, FIELD_SERIES_IDLE_STATE_RESIDENCY);

	if (!summary_mode && section_emit_cpu()) {
		residency |= get_enabled_series_mask(
			FIELD_SCOPE_CPU, FIELD_SERIES_IDLE_STATE_RESIDENCY);
		usage |= get_enabled_series_mask(
			FIELD_SCOPE_CPU, FIELD_SERIES_IDLE_STATE_USAGE);
	}

	if (residency_mask)
		*residency_mask = residency;
	if (usage_mask)
		*usage_mask = usage;
}

void set_section_summary_mode(int summary)
{
	summary_mode = summary;
}

void set_section_default_summary_output(int enable)
{
	default_summary_output = enable;
}
