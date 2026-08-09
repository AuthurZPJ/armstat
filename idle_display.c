/* SPDX-License-Identifier: GPL-2.0 */
/*
 * idle_display.c - LPI residual display rule
 *
 * See idle_display.h for the interface contract.
 */

#include <math.h>
#include <stddef.h>

#include "idle_display.h"

static double clamp_percent(double pct)
{
	if (pct < 0.0)
		return 0.0;
	if (pct > 100.0)
		return 100.0;
	return pct;
}

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
				double idle_pct,
				const int *visible,
				int summary_mode)
{
	double remaining = idle_pct;
	int last_state = -1;
	double hidden_value = summary_mode ? 0.0 : NAN;

	if (state_count > MAX_VISIBLE_IDLE_STATES)
		state_count = MAX_VISIBLE_IDLE_STATES;

	for (int s = state_count - 1; s >= 0; s--) {
		const struct idle_state *state =
			get_usable_state(idle_matrix, cpu_idx, s, state_count);

		if (state && visible[s]) {
			last_state = s;
			break;
		}
	}

	for (int s = 0; s < MAX_VISIBLE_IDLE_STATES; s++) {
		const struct idle_state *state =
			get_usable_state(idle_matrix, cpu_idx, s, state_count);
		double displayed;

		if (s >= state_count || !state || !visible[s]) {
			out[s] = hidden_value;
			continue;
		}

		if (s == last_state) {
			displayed = remaining;
		} else {
			displayed = clamp_percent(state->percentage);
			if (displayed > remaining)
				displayed = remaining;
			remaining -= displayed;
			if (remaining < 0.0)
				remaining = 0.0;
		}
		out[s] = clamp_percent(displayed);
	}
}
