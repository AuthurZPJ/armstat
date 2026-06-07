/* SPDX-License-Identifier: GPL-2.0 */
/*
 * aggregator.c - Aggregation and calculation layer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aggregator.h"
#include "power.h"
#include "pmu.h"
#include "topology.h"
#include "idle_backend.h"
#include "sysstat.h"

/* Previous raw counters for delta calculation */
static struct raw_counters prev_counters;
static int initialized;

/* Previous frequencies for interval average calculation */
static unsigned int prev_freqs[MAX_CPUS];
static unsigned long long prev_pmu_per_cpu[MAX_CPUS][MAX_PMU_EVENTS];
static unsigned long long prev_authoritative_idle_jiffies[MAX_CPUS];
static unsigned long long prev_authoritative_iowait_jiffies[MAX_CPUS];
static unsigned long long prev_authoritative_runtime_ns[MAX_CPUS];
static unsigned char prev_authoritative_runtime_valid[MAX_CPUS];

/* Current interval stats */
static struct interval_stats current_stats;

int init_aggregator(void)
{
	memset(&prev_counters, 0, sizeof(prev_counters));
	memset(&current_stats, 0, sizeof(current_stats));
	memset(prev_freqs, 0, sizeof(prev_freqs));
	memset(prev_pmu_per_cpu, 0, sizeof(prev_pmu_per_cpu));
	memset(prev_authoritative_idle_jiffies, 0,
	       sizeof(prev_authoritative_idle_jiffies));
	memset(prev_authoritative_iowait_jiffies, 0,
	       sizeof(prev_authoritative_iowait_jiffies));
	memset(prev_authoritative_runtime_ns, 0,
	       sizeof(prev_authoritative_runtime_ns));
	memset(prev_authoritative_runtime_valid, 0,
	       sizeof(prev_authoritative_runtime_valid));
	initialized = 1;

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
	int cpu_count = raw->effective_cpu_count;

	/* Warn about truncation once at start */
	static int warned_truncation = 0;
	if (raw->cpu_truncated && !warned_truncation) {
		fprintf(stderr, "WARNING: System has %d CPUs, only monitoring first %d (MAX_CPUS=%d)\n",
			raw->cpu_count, MAX_CPUS, MAX_CPUS);
		warned_truncation = 1;
	}

	/* Use unified time delta from collector for all metrics consistency */
	unsigned long long delta_us = raw->interval_delta_us;

	/* ===== 1. Calculate frequency statistics ===== */
	/* Per-CPU MHz and system-wide average */
	unsigned long total_freq = 0;
	int valid_cpus = 0;
	int i;

	/* Initialize per-cpu arrays */
	memset(stats->per_cpu_mhz, 0, sizeof(stats->per_cpu_mhz));
	memset(stats->per_cpu_busy, 0, sizeof(stats->per_cpu_busy));
	memset(stats->per_cpu_idle, 0, sizeof(stats->per_cpu_idle));
	memset(stats->per_cpu_iowait, 0, sizeof(stats->per_cpu_iowait));
	memset(stats->per_cpu_pmu, 0, sizeof(stats->per_cpu_pmu));
	memset(stats->per_cpu_ipc, 0, sizeof(stats->per_cpu_ipc));

	if (raw->freqs) {
		for (i = 0; i < cpu_count; i++) {
			unsigned int cur_freq = raw->freqs[i].cur_freq;
			if (cur_freq > 0) {
				/* Calculate interval average frequency for this CPU:
				 * avg = (previous + current) / 2 */
				unsigned int avg_freq;
				if (prev_freqs[i] > 0) {
					avg_freq = (prev_freqs[i] + cur_freq) / 2;
				} else {
					avg_freq = cur_freq;  /* First sample, use current */
				}
				stats->per_cpu_mhz[i] = avg_freq / 1000.0;
				total_freq += avg_freq;
				valid_cpus++;

				/* Store current as previous for next interval */
				prev_freqs[i] = cur_freq;
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
	if (raw->authoritative_idle_jiffies &&
	    raw->authoritative_iowait_jiffies &&
	    raw->authoritative_runtime_ns &&
	    delta_us > 0 && cpu_count > 0) {
		int hz = get_kernel_hz();

		for (i = 0; i < cpu_count; i++) {
			int cpu_id = get_cpu_id_by_tracked_idx(i);
			unsigned long long idle_delta_us = 0;
			unsigned long long iowait_delta_us = 0;
			double idle_pct;
			double iowait_pct;

			if (cpu_id >= 0 &&
			    busy_source_uses_schedstat_cpu(cpu_id) &&
			    raw->authoritative_runtime_valid &&
			    raw->authoritative_runtime_valid[i] &&
			    prev_authoritative_runtime_valid[i]) {
				unsigned long long runtime_delta_ns = 0;
				unsigned long long busy_us = 0;

				if (raw->authoritative_runtime_ns[i] >=
				    prev_authoritative_runtime_ns[i]) {
					runtime_delta_ns =
						raw->authoritative_runtime_ns[i] -
						prev_authoritative_runtime_ns[i];
				}
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
			} else {
				unsigned long long idle_delta_jiffies = 0;

				if (raw->authoritative_idle_jiffies[i] >=
				    prev_authoritative_idle_jiffies[i]) {
					idle_delta_jiffies =
						raw->authoritative_idle_jiffies[i] -
						prev_authoritative_idle_jiffies[i];
				}
				/*
				 * procstat idle is in jiffies. The jiffies counter
				 * cannot advance faster than wall clock, so
				 * idle_delta_jiffies * 1000000/hz is always <= delta_us.
				 * No explicit clamping needed here unlike the
				 * schedstat path where rounding can cause
				 * runtime_delta_ns to slightly exceed delta_us.
				 */
				idle_delta_us = (idle_delta_jiffies * 1000000ULL) / hz;
			}

			if (raw->authoritative_iowait_jiffies[i] >=
			    prev_authoritative_iowait_jiffies[i]) {
				unsigned long long iowait_delta_jiffies =
					raw->authoritative_iowait_jiffies[i] -
					prev_authoritative_iowait_jiffies[i];
				iowait_delta_us = (iowait_delta_jiffies * 1000000ULL) / hz;
			}
			/*
			 * If iowait went backwards (counter wrapped or CPU offline),
			 * we silently skip iowait for this interval rather than
			 * synthesizing a negative delta. This produces a flat 0%
			 * instead of a spike; the next interval recovers automatically.
			 */

			idle_pct = (double)idle_delta_us * 100.0 / (double)delta_us;
			iowait_pct = (double)iowait_delta_us * 100.0 / (double)delta_us;

			if (idle_pct < 0.0)
				idle_pct = 0.0;
			if (idle_pct > 100.0)
				idle_pct = 100.0;
			if (iowait_pct < 0.0)
				iowait_pct = 0.0;
			if (iowait_pct > 100.0)
				iowait_pct = 100.0;

			stats->per_cpu_idle[i] = idle_pct;
			stats->per_cpu_iowait[i] = iowait_pct;
			stats->per_cpu_busy[i] = 100.0 - idle_pct;
			total_idle += idle_pct;
			total_iowait += iowait_pct;
		}
		/* Average across tracked CPUs only (capped at MAX_CPUS)
		 * Note: this reflects only the CPUs we actually sampled */
		stats->avg_idle_percent = total_idle / cpu_count;
		stats->avg_iowait_percent = total_iowait / cpu_count;
		stats->busy_percent = 100.0 - stats->avg_idle_percent;
	}

	/* ===== 2. Memory bandwidth (calculated in aggregator using unified delta) ===== */
	/* Use pre-read mem_bw_counter from collector to avoid duplicate I/O */
	update_mem_bw_interval_stats(delta_us, raw->counters.mem_bw_counter);
	stats->mem_bw = get_interval_mem_bw();

	/* ===== 3. Power and energy (calculated in aggregator using unified delta) ===== */
	/* Use package-level power (always available) instead of summing per-CPU powers */
	long long current_total_power = raw->package_power_mw;
	update_power_interval_stats(delta_us, current_total_power);
	stats->avg_power_mw = get_interval_avg_power_mw();
	stats->interval_energy_joules = get_interval_energy_joules();

	/* ===== 4. System stat deltas ===== */
	if (delta_us > 0) {
		stats->ctx_switches =
			(raw->counters.ctx_switches >= prev_counters.ctx_switches) ?
			raw->counters.ctx_switches - prev_counters.ctx_switches : 0;
		stats->interrupts =
			(raw->counters.interrupts >= prev_counters.interrupts) ?
			raw->counters.interrupts - prev_counters.interrupts : 0;
		stats->soft_interrupts =
			(raw->counters.soft_interrupts >= prev_counters.soft_interrupts) ?
			raw->counters.soft_interrupts - prev_counters.soft_interrupts : 0;
	}

	/* ===== 5. PMU deltas ===== */
	int pmu_count = raw->counters.pmu_count;
	unsigned long long cycles = 0;
	unsigned long long instructions = 0;
	for (i = 0; i < pmu_count && i < MAX_PMU_EVENTS; i++) {
		stats->pmu_delta[i] = (raw->counters.pmu[i] >= prev_counters.pmu[i]) ?
			raw->counters.pmu[i] - prev_counters.pmu[i] : 0;
		/* Track cycles and instructions for IPC calculation */
		const char *name = get_pmu_event_name(i);
		if (name) {
			if (strcmp(name, "cycles") == 0)
				cycles = stats->pmu_delta[i];
			else if (strcmp(name, "instructions") == 0)
				instructions = stats->pmu_delta[i];
		}
	}

	/* Calculate IPC (Instructions Per Cycle) */
	stats->ipc = 0.0;
	if (cycles > 0) {
		stats->ipc = (double)instructions / cycles;
	}

	if (raw->counters.pmu_per_cpu && pmu_count > 0) {
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
			for (int event = 0; event < pmu_count && event < MAX_PMU_EVENTS; event++) {
				unsigned long long current = raw->counters.pmu_per_cpu[i][event];
				unsigned long long previous = prev_pmu_per_cpu[i][event];

				if (current >= previous)
					stats->per_cpu_pmu[i][event] = current - previous;
				else
					stats->per_cpu_pmu[i][event] = 0;

				prev_pmu_per_cpu[i][event] = current;
			}

			if (cycles_idx >= 0 && instructions_idx >= 0) {
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
		double pkg_freq_sum[MAX_PACKAGES] = {0};
		double pkg_idle_sum[MAX_PACKAGES] = {0};
		double pkg_iowait_sum[MAX_PACKAGES] = {0};
		int pkg_count = 0;

		for (int pkg_idx = 0; pkg_idx < MAX_PACKAGES; pkg_idx++)
			package_ids[pkg_idx] = -1;

		for (i = 0; i < cpu_count; i++) {
			int cpu_id = get_cpu_id_by_tracked_idx(i);
			int pkg = (cpu_id >= 0) ? get_package_id(cpu_id) : -1;
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
			pkg_freq_sum[pkg_idx]  += stats->per_cpu_mhz[i];
			pkg_idle_sum[pkg_idx]  += stats->per_cpu_idle[i];
			pkg_iowait_sum[pkg_idx] += stats->per_cpu_iowait[i];
		}

		stats->package_count = pkg_count;
		for (int idx = 0; idx < stats->package_count; idx++) {
			int n = pkg_cpu_counts[idx];
			stats->packages[idx].package_id = package_ids[idx];
			stats->packages[idx].cpu_count  = n;
			stats->packages[idx].avg_mhz    = (n > 0) ? pkg_freq_sum[idx] / n : 0;
			stats->packages[idx].idle_percent = (n > 0) ? pkg_idle_sum[idx] / n : 0;
			stats->packages[idx].busy_percent = 100.0 - stats->packages[idx].idle_percent;
			stats->packages[idx].iowait_percent = (n > 0) ? pkg_iowait_sum[idx] / n : 0;
		}
	}

	/* Save current counters as previous for next interval */
	prev_counters = raw->counters;
	for (i = 0; i < cpu_count; i++) {
		prev_authoritative_idle_jiffies[i] = raw->authoritative_idle_jiffies ?
			raw->authoritative_idle_jiffies[i] : 0;
		prev_authoritative_iowait_jiffies[i] = raw->authoritative_iowait_jiffies ?
			raw->authoritative_iowait_jiffies[i] : 0;
		prev_authoritative_runtime_ns[i] = raw->authoritative_runtime_ns ?
			raw->authoritative_runtime_ns[i] : 0;
		prev_authoritative_runtime_valid[i] =
			(raw->authoritative_runtime_valid &&
			 raw->authoritative_runtime_valid[i]) ? 1 : 0;
	}

	/* Copy to current_stats */
	current_stats = *stats;
}

void reset_aggregator(void)
{
	memset(&prev_counters, 0, sizeof(prev_counters));
	memset(&current_stats, 0, sizeof(current_stats));
	memset(prev_freqs, 0, sizeof(prev_freqs));
	memset(prev_pmu_per_cpu, 0, sizeof(prev_pmu_per_cpu));
	memset(prev_authoritative_idle_jiffies, 0,
	       sizeof(prev_authoritative_idle_jiffies));
	memset(prev_authoritative_iowait_jiffies, 0,
	       sizeof(prev_authoritative_iowait_jiffies));
	memset(prev_authoritative_runtime_ns, 0,
	       sizeof(prev_authoritative_runtime_ns));
	memset(prev_authoritative_runtime_valid, 0,
	       sizeof(prev_authoritative_runtime_valid));
	reset_energy();
	reset_mem_bw();
}

void cleanup_aggregator(void)
{
	initialized = 0;
}
