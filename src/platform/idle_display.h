/* SPDX-License-Identifier: GPL-2.0 */
/*
 * idle_display.h - LPI residual display rule
 *
 * Pure function that transforms raw cpuidle per-state residency percentages
 * into display percentages where the deepest visible usable state absorbs
 * the remainder so that sum(LPI-*) matches the authoritative Idle%.
 *
 * See docs/REFERENCE.md for the ARM-specific motivation (cpuidle stateN/time
 * counters only advance on state exit).
 */

#ifndef ARMSTAT_IDLE_DISPLAY_H
#define ARMSTAT_IDLE_DISPLAY_H

#include "columns.h"
#include "cpuidle.h"

/*
 * Compute display-adjusted per-idle-state residency for one CPU.
 *
 *   out[s]         - output: display percentage for state s
 *                    (NAN for incomplete current data; hidden/unusable states
 *                     are NAN in CPU mode and 0.0 in summary mode)
 *   idle_matrix    - per-CPU idle state array from the snapshot
 *                    (raw->idle); idle_matrix[cpu_idx] may be NULL
 *   cpu_idx        - tracked-CPU index into idle_matrix
 *   state_count    - number of idle states (raw->idle_state_count)
 *   idle_pct       - authoritative Idle% for this CPU (the target sum)
 *   visible        - visibility array (show_idle_state or
 *                    show_summary_idle_state), indexed by state
 *   summary_mode   - 0 = CPU-row mode (hidden = NAN),
 *                    1 = summary mode (hidden = 0.0)
 */
void compute_idle_state_display(double out[MAX_VISIBLE_IDLE_STATES],
				const struct idle_state **idle_matrix,
				int cpu_idx,
				int state_count,
				double idle_pct,
				const int *visible,
				int summary_mode);

#endif /* ARMSTAT_IDLE_DISPLAY_H */
