/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_CPUFREQ_H
#define ARMSTAT_CPUFREQ_H

#include <stddef.h>

struct cpu_freq_info {
	int cpu_id;
	unsigned int cur_freq;		/* kHz */
	unsigned int min_freq;		/* kHz */
	unsigned int max_freq;		/* kHz */
	int boost;			/* 0/1, or -1 if unavailable */
	char governor[32];
};

/* Read current CPU frequency in kHz (tracked_idx) */
int read_cpu_freq(int tracked_idx, unsigned int *freq);

/* Read min/max frequency (tracked_idx) */
int read_cpu_min_max_freq(int tracked_idx, unsigned int *min, unsigned int *max);

/* Read CPU governor (tracked_idx) */
int read_cpu_governor(int tracked_idx, char *governor, size_t len);

/* Read CPU boost state (0/1), returns -1 if unavailable (tracked_idx) */
int read_cpu_boost(int tracked_idx, int *boost);

/* Read uncore frequency from devfreq cur_freq in Hz */
int read_uncore_freq(unsigned long long *freq_hz);

/* Whether a readable devfreq cur_freq source was discovered */
int has_uncore_freq_support(void);

/* Return the discovered devfreq device name, or NULL if unavailable */
const char *get_uncore_freq_device_name(void);

/* Initialize cpufreq subsystem */
int init_cpufreq(void);

/* Close cached file descriptors */
void close_cpufreq(void);

#endif /* ARMSTAT_CPUFREQ_H */
