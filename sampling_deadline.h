/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_SAMPLING_DEADLINE_H
#define ARMSTAT_SAMPLING_DEADLINE_H

int sampling_deadline_init(unsigned long long sample_ns,
			   unsigned long long interval_ns,
			   unsigned long long *deadline_ns);
int sampling_deadline_advance(unsigned long long *deadline_ns,
			      unsigned long long interval_ns,
			      unsigned long long now_ns);

#endif /* ARMSTAT_SAMPLING_DEADLINE_H */
