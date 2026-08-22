/* SPDX-License-Identifier: GPL-2.0 */
/*
 * aggregator.c - Aggregation and calculation layer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <limits.h>

#include "aggregator.h"
#include "power.h"
#include "pmu.h"
#include "idle_backend.h"
#include "sysstat.h"
#include "cpu_inventory.h"

/* Previous raw counters for delta calculation */
static struct raw_counters prev_counters;

/* Previous frequencies for interval average calculation */
static unsigned int prev_freqs[MAX_CPUS];
static unsigned long long prev_pmu_per_cpu[MAX_CPUS][MAX_PMU_EVENTS];
static unsigned long long prev_authoritative_idle_jiffies[MAX_CPUS];
static unsigned long long prev_authoritative_iowait_jiffies[MAX_CPUS];
static unsigned char prev_authoritative_procstat_valid[MAX_CPUS];
static unsigned long long prev_authoritative_runtime_ns[MAX_CPUS];
static unsigned char prev_authoritative_runtime_valid[MAX_CPUS];

static int jiffies_delta_to_us(unsigned long long delta_jiffies, int hz,
			       unsigned long long *delta_us)
{
	if (!delta_us || hz <= 0 || delta_jiffies > ULLONG_MAX / 1000000ULL)
		return -1;

	*delta_us = (delta_jiffies * 1000000ULL) / (unsigned long long)hz;
	return 0;
}

int init_aggregator(void)
{
	memset(&prev_counters, 0, sizeof(prev_counters));
	memset(prev_freqs, 0, sizeof(prev_freqs));
	memset(prev_pmu_per_cpu, 0, sizeof(prev_pmu_per_cpu));
	memset(prev_authoritative_idle_jiffies, 0,
	       sizeof(prev_authoritative_idle_jiffies));
	memset(prev_authoritative_iowait_jiffies, 0,
	       sizeof(prev_authoritative_iowait_jiffies));
	memset(prev_authoritative_procstat_valid, 0,
	       sizeof(prev_authoritative_procstat_valid));
	memset(prev_authoritative_runtime_ns, 0,
	       sizeof(prev_authoritative_runtime_ns));
	memset(prev_authoritative_runtime_valid, 0,
	       sizeof(prev_authoritative_runtime_valid));

	/* Initialize power subsystem for energy tracking */
	reset_energy();
	reset_mem_bw();

	return 0;
}

void calculate_interval_stats(const struct sys_snapshot *raw, struct interval_stats *stats)
{
	if (!raw || !stats)
		return;

	/* Initialize output */
	memset(stats, 0, sizeof(*stats));

	/* Use effective CPU count from snapshot (already capped at MAX_CPUS) */
	int cpu_count = sys_snapshot_get_effective_cpu_count(raw);

	/* Warn about truncation once at start */
	static int warned_truncation = 0;
	if (sys_snapshot_get_cpu_truncated(raw) && !warned_truncation) {
		fprintf(stderr,
			"WARNING: System has %d online CPUs, monitoring %d representable CPUs (MAX_CPUS=%d)\n",
			raw->cpu_count, cpu_count, MAX_CPUS);
		warned_truncation = 1;
	}

	/* Use unified time delta from collector for all metrics consistency */
	unsigned long long delta_us = sys_snapshot_get_interval_delta_us(raw);

	/* ===== 1. Calculate frequency statistics ===== */
	/* Per-CPU MHz and system-wide average */
	unsigned long long total_freq = 0;
	int valid_cpus = 0;
	int i;

	/* Initialize per-cpu arrays */
	for (i = 0; i < MAX_CPUS; i++) {
		stats->per_cpu_mhz[i] = NAN;
		stats->per_cpu_busy[i] = NAN;
		stats->per_cpu_idle[i] = NAN;
		stats->per_cpu_iowait[i] = NAN;
		stats->per_cpu_ipc[i] = NAN;
	}
	memset(stats->per_cpu_pmu, 0, sizeof(stats->per_cpu_pmu));
	memset(stats->per_cpu_pmu_valid, 0,
	       sizeof(stats->per_cpu_pmu_valid));
	memset(stats->pmu_delta, 0, sizeof(stats->pmu_delta));
	stats->pmu_valid = 0;
	stats->avg_idle_percent = NAN;
	stats->avg_mhz = NAN;
	stats->avg_iowait_percent = NAN;
	stats->busy_percent = NAN;
	stats->ipc = NAN;
	stats->avg_power_mw = NAN;
	stats->interval_energy_joules = NAN;
	stats->mem_bw = NAN;
	stats->ctx_switches = NAN;
	stats->interrupts = NAN;
	stats->soft_interrupts = NAN;

	const struct cpu_freq_info *freqs = sys_snapshot_get_freqs(raw);
	if (freqs) {
		for (i = 0; i < cpu_count; i++) {
			unsigned int cur_freq = freqs[i].cur_freq;
			if (freqs[i].cur_freq_valid && cur_freq > 0) {
				/* Calculate interval average frequency for this CPU:
				 * avg = (previous + current) / 2 */
				unsigned long long avg_freq;
				if (prev_freqs[i] > 0) {
					avg_freq = ((unsigned long long)prev_freqs[i] +
						    cur_freq) / 2;
				} else {
					avg_freq = cur_freq;  /* First sample, use current */
				}
				stats->per_cpu_mhz[i] = avg_freq / 1000.0;
				total_freq += avg_freq;
				valid_cpus++;

				/* Store current as previous for next interval */
				prev_freqs[i] = cur_freq;
			} else {
				prev_freqs[i] = 0;
			}
		}
	}
	if (valid_cpus > 0) {
		stats->avg_mhz = (total_freq / (double)valid_cpus) / 1000.0;
	}

	/*
	 * Busy/Idle authority comes from raw procstat/schedstat counters captured
	 * in the snapshot. Aggregating them here keeps Busy/Idle on the same
	 * explicit delta timeline as PMU, power, and sysstat instead of relying on
	 * a separate backend-private "previous sample" state.
	 */
	double total_idle = 0;
	double total_iowait = 0;
	int valid_idle_cpus = 0;
	int valid_iowait_cpus = 0;
	if (raw->authoritative_idle_jiffies &&
	    raw->authoritative_iowait_jiffies &&
	    raw->authoritative_runtime_ns &&
	    delta_us > 0 && cpu_count > 0) {
		int hz = get_kernel_hz();
		if (hz <= 0)
			hz = 100;

		for (i = 0; i < cpu_count; i++) {
			int cpu_id = get_cpu_id_by_tracked_idx(i);
			unsigned long long idle_delta_us = 0;
			unsigned long long iowait_delta_us = 0;
			int procstat_valid = raw->authoritative_procstat_valid ?
				raw->authoritative_procstat_valid[i] : 1;
			int idle_valid = 0;
			int iowait_valid = 0;

			if (cpu_id >= 0 &&
			    busy_source_uses_schedstat_cpu(cpu_id) &&
			    raw->authoritative_runtime_valid &&
			    raw->authoritative_runtime_valid[i] &&
			    prev_authoritative_runtime_valid[i] &&
			    raw->authoritative_runtime_ns[i] >=
				prev_authoritative_runtime_ns[i]) {
				unsigned long long runtime_delta_ns = 0;
				unsigned long long busy_us = 0;

				runtime_delta_ns = raw->authoritative_runtime_ns[i] -
					prev_authoritative_runtime_ns[i];
				busy_us = runtime_delta_ns / 1000ULL;
				/*
				 * Clamp busy_us to delta_us. On nohz_full CPUs,
				 * schedstat runtime can occasionally exceed wall
				 * clock due to tickAccountingGranularity rounding,
				 * so we cap it here and derive idle as residual.
				 */
				if (busy_us > delta_us)
					busy_us = delta_us;
				idle_delta_us = delta_us - busy_us;
				idle_valid = 1;
			} else if (procstat_valid &&
				   prev_authoritative_procstat_valid[i] &&
				   raw->authoritative_idle_jiffies[i] >=
					prev_authoritative_idle_jiffies[i]) {
				unsigned long long idle_delta_jiffies =
					raw->authoritative_idle_jiffies[i] -
					prev_authoritative_idle_jiffies[i];

				if (jiffies_delta_to_us(idle_delta_jiffies, hz,
							 &idle_delta_us) == 0) {
					if (idle_delta_us > delta_us)
						idle_delta_us = delta_us;
					idle_valid = 1;
				}
			}

			if (procstat_valid && prev_authoritative_procstat_valid[i] &&
			    raw->authoritative_iowait_jiffies[i] >=
				prev_authoritative_iowait_jiffies[i]) {
				unsigned long long iowait_delta_jiffies =
					raw->authoritative_iowait_jiffies[i] -
					prev_authoritative_iowait_jiffies[i];
				if (jiffies_delta_to_us(iowait_delta_jiffies, hz,
							 &iowait_delta_us) == 0) {
					if (iowait_delta_us > delta_us)
						iowait_delta_us = delta_us;
					iowait_valid = 1;
				}
			}

			if (idle_valid) {
				double idle_pct = (double)idle_delta_us * 100.0 /
					(double)delta_us;

				stats->per_cpu_idle[i] = idle_pct;
				stats->per_cpu_busy[i] = 100.0 - idle_pct;
				total_idle += idle_pct;
				valid_idle_cpus++;
			}
			if (iowait_valid) {
				double iowait_pct = (double)iowait_delta_us * 100.0 /
					(double)delta_us;

				stats->per_cpu_iowait[i] = iowait_pct;
				total_iowait += iowait_pct;
				valid_iowait_cpus++;
			}
		}
		if (valid_idle_cpus > 0) {
			stats->avg_idle_percent = total_idle / valid_idle_cpus;
			stats->busy_percent = 100.0 - stats->avg_idle_percent;
		}
		if (valid_iowait_cpus > 0)
			stats->avg_iowait_percent = total_iowait / valid_iowait_cpus;
	}

	/* ===== 2. Memory bandwidth (calculated in aggregator using unified delta) ===== */
	struct raw_counters current_counters = sys_snapshot_get_counters(raw);
	/* Use pre-read mem_bw_counter from collector to avoid duplicate I/O */
	update_mem_bw_interval_stats(delta_us, current_counters.mem_bw_counter,
				     current_counters.mem_bw_valid);
	stats->mem_bw = get_interval_mem_bw();

	/* ===== 3. Power and energy (calculated in aggregator using unified delta) ===== */
	/* Use package-level power when available instead of summing per-CPU powers. */
	long long current_total_power = raw->package_power_mw;
	update_power_interval_stats(delta_us,
				    current_total_power >= 0 ?
					(unsigned long long)current_total_power : 0,
				    raw->package_power_valid &&
					current_total_power >= 0);
	stats->avg_power_mw = get_interval_avg_power_mw();
	stats->interval_energy_joules = get_interval_energy_joules();

	/* ===== 4. System stat deltas ===== */
	if (delta_us > 0 && current_counters.sysstat_valid &&
	    prev_counters.sysstat_valid) {
		if (current_counters.ctx_switches >= prev_counters.ctx_switches)
			stats->ctx_switches = current_counters.ctx_switches -
				prev_counters.ctx_switches;
		if (current_counters.interrupts >= prev_counters.interrupts)
			stats->interrupts = current_counters.interrupts -
				prev_counters.interrupts;
		if (current_counters.soft_interrupts >=
		    prev_counters.soft_interrupts)
			stats->soft_interrupts = current_counters.soft_interrupts -
				prev_counters.soft_interrupts;
	}

	/* ===== 5. PMU deltas ===== */
	int pmu_count = current_counters.pmu_count;
	unsigned long long cycles = 0;
	unsigned long long instructions = 0;
	int have_cycles = 0;
	int have_instructions = 0;
	for (i = 0; current_counters.pmu_valid &&
		     i < pmu_count && i < MAX_PMU_EVENTS; i++) {
		if (current_counters.pmu[i] < prev_counters.pmu[i])
			break;
		stats->pmu_delta[i] = current_counters.pmu[i] -
			prev_counters.pmu[i];
		/* Track cycles and instructions for IPC calculation */
		const char *name = get_pmu_event_name(i);
		if (name) {
			if (strcmp(name, "cycles") == 0) {
				cycles = stats->pmu_delta[i];
				have_cycles = 1;
			} else if (strcmp(name, "instructions") == 0) {
				instructions = stats->pmu_delta[i];
				have_instructions = 1;
			}
		}
	}
	if (pmu_count > 0 && current_counters.pmu_valid && i == pmu_count)
		stats->pmu_valid = 1;

	/* Calculate IPC (Instructions Per Cycle) */
	if (stats->pmu_valid && have_cycles && have_instructions && cycles > 0) {
		stats->ipc = (double)instructions / cycles;
	}

	if (current_counters.pmu_per_cpu && pmu_count > 0) {
		int cycles_idx = -1;
		int instructions_idx = -1;

		for (i = 0; i < pmu_count && i < MAX_PMU_EVENTS; i++) {
			const char *name = get_pmu_event_name(i);

			if (!name)
				continue;
			if (strcmp(name, "cycles") == 0)
				cycles_idx = i;
			else if (strcmp(name, "instructions") == 0)
				instructions_idx = i;
		}

		for (i = 0; i < cpu_count; i++) {
			int cpu_valid = current_counters.pmu_per_cpu_valid &&
				current_counters.pmu_per_cpu_valid[i];

			for (int event = 0; event < pmu_count && event < MAX_PMU_EVENTS; event++) {
				unsigned long long current = current_counters.pmu_per_cpu[i][event];
				unsigned long long previous = prev_pmu_per_cpu[i][event];

				if (cpu_valid && current >= previous)
					stats->per_cpu_pmu[i][event] = current - previous;
				else if (cpu_valid)
					cpu_valid = 0;

				prev_pmu_per_cpu[i][event] = current;
			}
			stats->per_cpu_pmu_valid[i] = cpu_valid;

			if (cpu_valid && cycles_idx >= 0 && instructions_idx >= 0) {
				unsigned long long cpu_cycles = stats->per_cpu_pmu[i][cycles_idx];
				unsigned long long cpu_instructions = stats->per_cpu_pmu[i][instructions_idx];

				if (cpu_cycles > 0)
					stats->per_cpu_ipc[i] =
						(double)cpu_instructions / cpu_cycles;
			}
		}
	}

	/* ===== 6. Per-package aggregation ===== */
	{
		int package_ids[MAX_PACKAGES];
		int pkg_cpu_counts[MAX_PACKAGES] = {0};
		int pkg_freq_counts[MAX_PACKAGES] = {0};
		int pkg_idle_counts[MAX_PACKAGES] = {0};
		int pkg_iowait_counts[MAX_PACKAGES] = {0};
		double pkg_freq_sum[MAX_PACKAGES] = {0};
		double pkg_idle_sum[MAX_PACKAGES] = {0};
		double pkg_iowait_sum[MAX_PACKAGES] = {0};
		int pkg_count = 0;
		struct cpu_desc *desc;

		for (int pkg_idx = 0; pkg_idx < MAX_PACKAGES; pkg_idx++)
			package_ids[pkg_idx] = -1;

		for_each_tracked_cpu(i, desc) {
			int pkg = desc->package_id;
			int pkg_idx = -1;

			if (pkg < 0)
				continue;

			for (int idx = 0; idx < pkg_count; idx++) {
				if (package_ids[idx] == pkg) {
					pkg_idx = idx;
					break;
				}
			}

			if (pkg_idx < 0) {
				if (pkg_count >= MAX_PACKAGES)
					continue;
				pkg_idx = pkg_count++;
				package_ids[pkg_idx] = pkg;
			}

			pkg_cpu_counts[pkg_idx]++;
			if (!isnan(stats->per_cpu_mhz[i])) {
				pkg_freq_sum[pkg_idx] += stats->per_cpu_mhz[i];
				pkg_freq_counts[pkg_idx]++;
			}
			if (!isnan(stats->per_cpu_idle[i])) {
				pkg_idle_sum[pkg_idx] += stats->per_cpu_idle[i];
				pkg_idle_counts[pkg_idx]++;
			}
			if (!isnan(stats->per_cpu_iowait[i])) {
				pkg_iowait_sum[pkg_idx] += stats->per_cpu_iowait[i];
				pkg_iowait_counts[pkg_idx]++;
			}
		}

		stats->package_count = pkg_count;
		for (int idx = 0; idx < stats->package_count; idx++) {
			int n = pkg_cpu_counts[idx];
			stats->packages[idx].package_id = package_ids[idx];
			stats->packages[idx].cpu_count  = n;
			stats->packages[idx].avg_mhz = pkg_freq_counts[idx] > 0 ?
				pkg_freq_sum[idx] / pkg_freq_counts[idx] : NAN;
			stats->packages[idx].idle_percent = pkg_idle_counts[idx] > 0 ?
				pkg_idle_sum[idx] / pkg_idle_counts[idx] : NAN;
			stats->packages[idx].busy_percent = pkg_idle_counts[idx] > 0 ?
				100.0 - stats->packages[idx].idle_percent : NAN;
			stats->packages[idx].iowait_percent = pkg_iowait_counts[idx] > 0 ?
				pkg_iowait_sum[idx] / pkg_iowait_counts[idx] : NAN;
		}
	}

	/* Save current counters as previous for next interval */
	prev_counters = current_counters;
	for (i = 0; i < cpu_count; i++) {
		prev_authoritative_idle_jiffies[i] = raw->authoritative_idle_jiffies ?
			raw->authoritative_idle_jiffies[i] : 0;
		prev_authoritative_iowait_jiffies[i] = raw->authoritative_iowait_jiffies ?
			raw->authoritative_iowait_jiffies[i] : 0;
		prev_authoritative_procstat_valid[i] =
			(raw->authoritative_procstat_valid ?
			 raw->authoritative_procstat_valid[i] :
			 (raw->authoritative_idle_jiffies != NULL)) ? 1 : 0;
		prev_authoritative_runtime_ns[i] = raw->authoritative_runtime_ns ?
			raw->authoritative_runtime_ns[i] : 0;
		prev_authoritative_runtime_valid[i] =
			(raw->authoritative_runtime_valid &&
			 raw->authoritative_runtime_valid[i]) ? 1 : 0;
	}
}

void reset_aggregator(void)
{
	memset(&prev_counters, 0, sizeof(prev_counters));
	memset(prev_freqs, 0, sizeof(prev_freqs));
	memset(prev_pmu_per_cpu, 0, sizeof(prev_pmu_per_cpu));
	memset(prev_authoritative_idle_jiffies, 0,
	       sizeof(prev_authoritative_idle_jiffies));
	memset(prev_authoritative_iowait_jiffies, 0,
	       sizeof(prev_authoritative_iowait_jiffies));
	memset(prev_authoritative_procstat_valid, 0,
	       sizeof(prev_authoritative_procstat_valid));
	memset(prev_authoritative_runtime_ns, 0,
	       sizeof(prev_authoritative_runtime_ns));
	memset(prev_authoritative_runtime_valid, 0,
	       sizeof(prev_authoritative_runtime_valid));
	reset_energy();
	reset_mem_bw();
}

void cleanup_aggregator(void)
{
}
