/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_section.h - Section emission policy
 *
 * Single home for the "which output section is emitted this interval"
 * decisions shared by the text, JSON, and CSV serializers, plus the
 * summary-mode state those decisions depend on.
 */

#ifndef ARMSTAT_FORMATTER_SECTION_H
#define ARMSTAT_FORMATTER_SECTION_H

/* Whether per-CPU rows should be emitted. */
int section_emit_cpu(void);

/* Whether per-package aggregation rows should be emitted. */
int section_emit_package(void);

/*
 * Whether the CPU identity column should be present so per-CPU rows stay
 * self-describing even when the cpu group is hidden.
 */
int section_emit_cpu_identity(void);

/* Whether the default SUM section should be emitted (non -S summary output). */
int section_emit_default_summary(void);

/* Whether CSV should use a mixed SUM/package/CPU scope layout. */
int section_emit_mixed_csv(void);

/* Summary-mode state (set by -S). */
int section_is_summary_mode(void);

/* Default-summary-output state (set by -a). */
int section_default_summary_output(void);

/* Whether the configured mode can emit at least one meaningful data row. */
int section_has_output(void);

void set_section_summary_mode(int summary);
void set_section_default_summary_output(int enable);

#endif /* ARMSTAT_FORMATTER_SECTION_H */
