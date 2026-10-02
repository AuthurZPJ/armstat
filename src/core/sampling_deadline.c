/* SPDX-License-Identifier: GPL-2.0 */
/* Sampling cadence and interval arithmetic, kept independent of clock I/O. */

#include <limits.h>

#include "sampling_deadline.h"

unsigned long long sampling_interval_delta_us(unsigned long long previous_us,
					      unsigned long long current_us,
					      int reset_baseline)
{
	if (reset_baseline || previous_us == 0 || current_us <= previous_us)
		return 0;

	return current_us - previous_us;
}

int sampling_deadline_init(unsigned long long sample_ns,
			   unsigned long long interval_ns,
			   unsigned long long *deadline_ns)
{
	if (!deadline_ns || interval_ns == 0 ||
	    sample_ns > ULLONG_MAX - interval_ns)
		return -1;

	*deadline_ns = sample_ns + interval_ns;
	return 0;
}

int sampling_deadline_advance(unsigned long long *deadline_ns,
			      unsigned long long interval_ns,
			      unsigned long long now_ns)
{
	unsigned long long next_ns;
	unsigned long long skipped;

	if (!deadline_ns || interval_ns == 0 ||
	    *deadline_ns > ULLONG_MAX - interval_ns)
		return -1;

	next_ns = *deadline_ns + interval_ns;
	if (next_ns <= now_ns) {
		skipped = (now_ns - next_ns) / interval_ns + 1;
		if (skipped > (ULLONG_MAX - next_ns) / interval_ns)
			return -1;
		next_ns += skipped * interval_ns;
	}

	*deadline_ns = next_ns;
	return 0;
}
