/* SPDX-License-Identifier: GPL-2.0 */
/*
 * idle_display.c - Select independent cpuidle interval percentages
 *
 * See idle_display.h for the interface contract.
 */

#include <math.h>
#include <stddef.h>

#include "idle_display.h"

static const struct idle_state *get_usable_state(
	const struct idle_state **idle_matrix,
	int cpu_idx, int state_idx, int state_count)
{
	if (!idle_matrix || cpu_idx < 0)
		return NULL;
	if (state_idx < 0 || state_idx >= state_count)
		return NULL;
	if (!idle_matrix[cpu_idx])
		return NULL;

	{
		const struct idle_state *state = &idle_matrix[cpu_idx][state_idx];

		if (!state->available || state->disabled)
			return NULL;
		return state;
	}
}

void compute_idle_state_display(double out[MAX_VISIBLE_IDLE_STATES],
				const struct idle_state **idle_matrix,
				int cpu_idx,
				int state_count,
				const int *visible)
{
	if (state_count > MAX_VISIBLE_IDLE_STATES)
		state_count = MAX_VISIBLE_IDLE_STATES;

	for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++) {
		const struct idle_state *state =
			get_usable_state(idle_matrix, cpu_idx, s, state_count);

		out[s] = state && visible && visible[s] &&
			isfinite(state->percentage) && state->percentage >= 0.0 ?
			state->percentage : NAN;
	}
}
