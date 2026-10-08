/* SPDX-License-Identifier: GPL-2.0 */
/*
 * idle_display.h - Independent cpuidle interval percentages
 *
 * Pure function that selects usable state percentages without redistributing
 * time between states or normalizing to Idle%. Delayed state-exit accounting
 * can produce percentages above 100; preserve them as reported by the backend.
 */

#ifndef ARMSTAT_IDLE_DISPLAY_H
#define ARMSTAT_IDLE_DISPLAY_H

#include "columns.h"
#include "cpuidle.h"

/*
 * Select per-idle-state residency for one CPU.
 *
 *   out[s]         - output: display percentage for state s
 *                    (NAN for hidden, unusable, or invalid states)
 *   idle_matrix    - per-CPU idle state array from the snapshot
 *                    (raw->idle); idle_matrix[cpu_idx] may be NULL
 *   cpu_idx        - tracked-CPU index into idle_matrix
 *   state_count    - number of idle states (raw->idle_state_count)
 *   visible        - visibility array (show_idle_state or
 *                    show_summary_idle_state), indexed by state
 */
void compute_idle_state_display(double out[MAX_VISIBLE_IDLE_STATES],
				const struct idle_state **idle_matrix,
				int cpu_idx,
				int state_count,
				const int *visible);

#endif /* ARMSTAT_IDLE_DISPLAY_H */
