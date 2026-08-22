/* SPDX-License-Identifier: GPL-2.0 */
/*
 * aggregator.c - Aggregation and calculation layer
 */

#include <stdio.h>
#include <string.h>
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

static unsigned long long prev_pmu_per_cpu[MAX_CPUS][MAX_PMU_EVENTS];
static unsigned long long prev_authoritative_idle_jiffies[MAX_CPUS];
static unsigned long long prev_authoritative_iowait_jiffies[MAX_CPUS];
static unsigned char prev_authoritative_procstat_valid[MAX_CPUS];
static unsigned long long prev_authoritative_runtime_ns[MAX_CPUS];
static unsigned char prev_authoritative_runtime_valid[MAX_CPUS];

struct package_accumulator {
	int package_id;
	int cpu_count;
	int freq_count;
	int idle_count;
	int iowait_count;
	double freq_sum;
	double idle_sum;
	double iowait_sum;
};

static void clear_aggregator_baselines(void)
{
	memset(&prev_counters, 0, sizeof(prev_counters));
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
}

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
	clear_aggregator_baselines();

	/* Initialize power subsystem for energy tracking */
	reset_energy();
	reset_mem_bw();

	return 0;
}

static void initialize_interval_stats(struct interval_stats *stats)
{
	memset(stats, 0, sizeof(*stats));

	for (int i = 0; i < MAX_CPUS; i++) {
		stats->per_cpu_mhz[i] = NAN;
		stats->per_cpu_busy[i] = NAN;
		stats->per_cpu_idle[i] = NAN;
		stats->per_cpu_iowait[i] = NAN;
		stats->per_cpu_ipc[i] = NAN;
	}

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
}

static void calculate_frequency_stats(const struct sys_snapshot *raw,
				      struct interval_stats *stats,
				      int cpu_count)
{
	const struct cpu_freq_info *freqs = sys_snapshot_get_freqs(raw);
	unsigned long long total_freq = 0;
	int valid_cpus = 0;

	if (!freqs)
		return;

	for (int i = 0; i < cpu_count; i++) {
		unsigned int cur_freq = freqs[i].cur_freq;

		if (!freqs[i].cur_freq_valid || cur_freq == 0)
			continue;

		/* A point sample, aggregated across the current tracked CPU set. */
		stats->per_cpu_mhz[i] = cur_freq / 1000.0;
		total_freq += cur_freq;
		valid_cpus++;
	}

	if (valid_cpus > 0)
		stats->avg_mhz = (total_freq / (double)valid_cpus) / 1000.0;
}

static void warn_cpu_truncation(const struct sys_snapshot *raw, int cpu_count)
{
	static int warned;

	if (!sys_snapshot_get_cpu_truncated(raw) || warned)
		return;

	fprintf(stderr,
		"WARNING: System has %d online CPUs, monitoring %d representable CPUs (MAX_CPUS=%d)\n",
		raw->cpu_count, cpu_count, MAX_CPUS);
	warned = 1;
}

static void calculate_busy_idle_stats(const struct sys_snapshot *raw,
				      struct interval_stats *stats,
				      int cpu_count,
				      unsigned long long delta_us)
{
	double total_idle = 0;
	double total_iowait = 0;
	int valid_idle_cpus = 0;
	int valid_iowait_cpus = 0;
	int hz;

	/*
	 * Busy/Idle authority comes from raw procstat/schedstat counters captured
	 * in the snapshot. Aggregating them here keeps Busy/Idle on the same
	 * explicit delta timeline as PMU, power, and sysstat instead of relying on
	 * a separate backend-private "previous sample" state.
	 */
	if (!raw->authoritative_idle_jiffies ||
	    !raw->authoritative_iowait_jiffies ||
	    !raw->authoritative_runtime_ns || delta_us == 0 || cpu_count <= 0)
		return;

	hz = get_kernel_hz();
	if (hz <= 0)
		hz = 100;

	for (int i = 0; i < cpu_count; i++) {
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
			unsigned long long runtime_delta_ns;
			unsigned long long busy_us;

			runtime_delta_ns = raw->authoritative_runtime_ns[i] -
				prev_authoritative_runtime_ns[i];
			busy_us = runtime_delta_ns / 1000ULL;
			/*
			 * On nohz_full CPUs, schedstat rounding can make runtime
			 * slightly exceed wall time. Derive idle from a clamped value.
			 */
			if (busy_us > delta_us)
				busy_us = delta_us;
			idle_delta_us = delta_us - busy_us;
			idle_valid = 1;
		} else if (procstat_valid &&
			   prev_authoritative_procstat_valid[i] &&
			   raw->authoritative_idle_jiffies[i] >=
				prev_authoritative_idle_jiffies[i]) {
			unsigned long long idle_delta_jiffies;

			idle_delta_jiffies = raw->authoritative_idle_jiffies[i] -
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
			unsigned long long iowait_delta_jiffies;

			iowait_delta_jiffies = raw->authoritative_iowait_jiffies[i] -
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

static void calculate_system_stats(const struct sys_snapshot *raw,
				   const struct raw_counters *current,
				   struct interval_stats *stats,
				   unsigned long long delta_us)
{
	long long current_total_power;

	/* Use pre-read mem_bw_counter from collector to avoid duplicate I/O */
	update_mem_bw_interval_stats(delta_us, current->mem_bw_counter,
				     current->mem_bw_valid);
	stats->mem_bw = get_interval_mem_bw();

	/* Use package-level power when available instead of summing per-CPU powers. */
	current_total_power = raw->package_power_mw;
	update_power_interval_stats(delta_us,
				    current_total_power >= 0 ?
					(unsigned long long)current_total_power : 0,
				    raw->package_power_valid &&
					current_total_power >= 0);
	stats->avg_power_mw = get_interval_avg_power_mw();
	stats->interval_energy_joules = get_interval_energy_joules();

	if (delta_us > 0 && current->sysstat_valid &&
	    prev_counters.sysstat_valid) {
		if (current->ctx_switches >= prev_counters.ctx_switches)
			stats->ctx_switches = current->ctx_switches -
				prev_counters.ctx_switches;
		if (current->interrupts >= prev_counters.interrupts)
			stats->interrupts = current->interrupts -
				prev_counters.interrupts;
		if (current->soft_interrupts >=
		    prev_counters.soft_interrupts)
			stats->soft_interrupts = current->soft_interrupts -
				prev_counters.soft_interrupts;
	}
}

static void find_pmu_event_indexes(int pmu_count, int *cycles_idx,
				   int *instructions_idx)
{
	*cycles_idx = -1;
	*instructions_idx = -1;

	for (int i = 0; i < pmu_count && i < MAX_PMU_EVENTS; i++) {
		const char *name = get_pmu_event_name(i);

		if (!name)
			continue;
		if (strcmp(name, "cycles") == 0)
			*cycles_idx = i;
		else if (strcmp(name, "instructions") == 0)
			*instructions_idx = i;
	}
}

static void calculate_summary_pmu_stats(const struct raw_counters *current,
					struct interval_stats *stats,
					int cycles_idx,
					int instructions_idx)
{
	int pmu_count = current->pmu_count;
	int event;

	for (event = 0; current->pmu_valid && event < pmu_count &&
	     event < MAX_PMU_EVENTS; event++) {
		if (current->pmu[event] < prev_counters.pmu[event])
			break;
		stats->pmu_delta[event] = current->pmu[event] -
			prev_counters.pmu[event];
	}
	if (pmu_count > 0 && current->pmu_valid && event == pmu_count)
		stats->pmu_valid = 1;

	if (stats->pmu_valid && cycles_idx >= 0 && instructions_idx >= 0 &&
	    stats->pmu_delta[cycles_idx] > 0)
		stats->ipc = (double)stats->pmu_delta[instructions_idx] /
			stats->pmu_delta[cycles_idx];
}

static void calculate_per_cpu_pmu_stats(const struct raw_counters *current,
					struct interval_stats *stats,
					int cpu_count, int cycles_idx,
					int instructions_idx)
{
	int pmu_count = current->pmu_count;

	if (!current->pmu_per_cpu || pmu_count <= 0)
		return;

	for (int cpu = 0; cpu < cpu_count; cpu++) {
		int cpu_valid = current->pmu_per_cpu_valid &&
			current->pmu_per_cpu_valid[cpu];

		for (int event = 0; event < pmu_count &&
		     event < MAX_PMU_EVENTS; event++) {
			unsigned long long value = current->pmu_per_cpu[cpu][event];
			unsigned long long previous = prev_pmu_per_cpu[cpu][event];

			if (cpu_valid && value >= previous)
				stats->per_cpu_pmu[cpu][event] = value - previous;
			else if (cpu_valid)
				cpu_valid = 0;

			prev_pmu_per_cpu[cpu][event] = value;
		}
		stats->per_cpu_pmu_valid[cpu] = cpu_valid;

		if (cpu_valid && cycles_idx >= 0 && instructions_idx >= 0) {
			unsigned long long cycles =
				stats->per_cpu_pmu[cpu][cycles_idx];
			unsigned long long instructions =
				stats->per_cpu_pmu[cpu][instructions_idx];

			if (cycles > 0)
				stats->per_cpu_ipc[cpu] =
					(double)instructions / cycles;
		}
	}
}

static void calculate_pmu_stats(const struct raw_counters *current,
				struct interval_stats *stats, int cpu_count)
{
	int cycles_idx;
	int instructions_idx;

	find_pmu_event_indexes(current->pmu_count, &cycles_idx,
			       &instructions_idx);
	calculate_summary_pmu_stats(current, stats, cycles_idx, instructions_idx);
	calculate_per_cpu_pmu_stats(current, stats, cpu_count, cycles_idx,
				    instructions_idx);
}

static struct package_accumulator *get_package_accumulator(
	struct package_accumulator *packages, int *package_count, int package_id)
{
	for (int i = 0; i < *package_count; i++) {
		if (packages[i].package_id == package_id)
			return &packages[i];
	}

	if (*package_count >= MAX_PACKAGES)
		return NULL;

	packages[*package_count].package_id = package_id;
	return &packages[(*package_count)++];
}

static void calculate_package_stats(struct interval_stats *stats)
{
	struct package_accumulator packages[MAX_PACKAGES] = {0};
	struct cpu_desc *desc;
	int package_count = 0;
	int i;

	for_each_tracked_cpu(i, desc) {
		struct package_accumulator *package;

		if (desc->package_id < 0)
			continue;

		package = get_package_accumulator(packages, &package_count,
						      desc->package_id);
		if (!package)
			continue;

		package->cpu_count++;
		if (!isnan(stats->per_cpu_mhz[i])) {
			package->freq_sum += stats->per_cpu_mhz[i];
			package->freq_count++;
		}
		if (!isnan(stats->per_cpu_idle[i])) {
			package->idle_sum += stats->per_cpu_idle[i];
			package->idle_count++;
		}
		if (!isnan(stats->per_cpu_iowait[i])) {
			package->iowait_sum += stats->per_cpu_iowait[i];
			package->iowait_count++;
		}
	}

	stats->package_count = package_count;
	for (i = 0; i < stats->package_count; i++) {
		struct package_accumulator *package = &packages[i];

		stats->packages[i].package_id = package->package_id;
		stats->packages[i].cpu_count = package->cpu_count;
		stats->packages[i].avg_mhz = package->freq_count > 0 ?
			package->freq_sum / package->freq_count : NAN;
		stats->packages[i].idle_percent = package->idle_count > 0 ?
			package->idle_sum / package->idle_count : NAN;
		stats->packages[i].busy_percent = package->idle_count > 0 ?
			100.0 - stats->packages[i].idle_percent : NAN;
		stats->packages[i].iowait_percent = package->iowait_count > 0 ?
			package->iowait_sum / package->iowait_count : NAN;
	}
}

static void save_aggregator_baseline(const struct sys_snapshot *raw,
				     const struct raw_counters *current,
				     int cpu_count)
{
	prev_counters = *current;

	for (int i = 0; i < cpu_count; i++) {
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

void calculate_interval_stats(const struct sys_snapshot *raw,
			      struct interval_stats *stats)
{
	struct raw_counters current;
	unsigned long long delta_us;
	int cpu_count;

	if (!raw || !stats)
		return;

	cpu_count = sys_snapshot_get_effective_cpu_count(raw);
	delta_us = sys_snapshot_get_interval_delta_us(raw);
	current = sys_snapshot_get_counters(raw);

	warn_cpu_truncation(raw, cpu_count);
	initialize_interval_stats(stats);
	calculate_frequency_stats(raw, stats, cpu_count);
	calculate_busy_idle_stats(raw, stats, cpu_count, delta_us);
	calculate_system_stats(raw, &current, stats, delta_us);
	calculate_pmu_stats(&current, stats, cpu_count);
	calculate_package_stats(stats);
	save_aggregator_baseline(raw, &current, cpu_count);
}

void reset_aggregator(void)
{
	clear_aggregator_baselines();
	reset_energy();
	reset_mem_bw();
}

void cleanup_aggregator(void)
{
}
