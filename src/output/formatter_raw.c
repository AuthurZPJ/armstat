/* SPDX-License-Identifier: GPL-2.0 */
/* Raw snapshot output: no interval differences, rates, or percentages. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "formatter_raw.h"
#include "collector.h"
#include "cpu_inventory.h"
#include "power.h"
#include "sysstat.h"

static void print_raw_unsigned(unsigned long long value, int valid, int width)
{
	if (valid)
		printf("%*llu", width, value);
	else
		printf("%*s", width, "-");
}

static void print_raw_signed(long long value, int valid, int width)
{
	if (valid)
		printf("%*lld", width, value);
	else
		printf("%*s", width, "-");
}

static const char *raw_string(const char *value)
{
	return value && *value ? value : "-";
}

static void print_raw_cpu_identity(int tracked_idx)
{
	int cpu_id = get_cpu_id_by_tracked_idx(tracked_idx);

	print_raw_signed(cpu_id, cpu_id >= 0, 6);
}

static void print_raw_metadata(const struct sys_snapshot *snapshot)
{
	printf("armstat raw snapshot\n");
	printf("  '-' means unavailable or not collected; zero is a valid reading.\n");
	printf("  Counters are cumulative since boot/source reset; sensors are current.\n");
	printf("  sample_realtime_ns: ");
	print_raw_unsigned(snapshot->sample_timestamp_ns,
			   snapshot->sample_timestamp_ns != 0, 0);
	printf("\n  sample_monotonic_ns: ");
	print_raw_unsigned(snapshot->sample_monotonic_ns,
			   snapshot->sample_monotonic_ns != 0, 0);
	printf("\n  online_cpus: %d\n  tracked_cpus: %d\n  cpu_truncated: %s\n",
	       snapshot->cpu_count, snapshot->effective_cpu_count,
	       snapshot->cpu_truncated ? "yes" : "no");
}

static void print_raw_procstat(const struct sys_snapshot *snapshot)
{
	const struct raw_counters *counters = &snapshot->counters;
	int count = snapshot->effective_cpu_count;

	printf("\n/proc/stat (cumulative; USER_HZ=%d ticks/s)\n", get_kernel_hz());
	printf("  ctxt[count]: ");
	print_raw_unsigned(counters->ctx_switches, counters->sysstat_valid, 0);
	printf("\n  intr[count]: ");
	print_raw_unsigned(counters->interrupts, counters->sysstat_valid, 0);
	printf("\n  softirq[count]: ");
	print_raw_unsigned(counters->soft_interrupts, counters->sysstat_valid, 0);
	printf("\n  Idle+IOWait is the collected sum of the idle and iowait fields.\n");
	printf("%6s %22s %22s\n", "CPU", "Idle+IOWait[jiffies]", "IOWait[jiffies]");
	for (int i = 0; i < count; i++) {
		int valid = snapshot->authoritative_procstat_valid &&
			    snapshot->authoritative_procstat_valid[i];

		print_raw_cpu_identity(i);
		putchar(' ');
		print_raw_unsigned(snapshot->authoritative_idle_jiffies ?
				   snapshot->authoritative_idle_jiffies[i] : 0,
				   valid && snapshot->authoritative_idle_jiffies, 22);
		putchar(' ');
		print_raw_unsigned(snapshot->authoritative_iowait_jiffies ?
				   snapshot->authoritative_iowait_jiffies[i] : 0,
				   valid && snapshot->authoritative_iowait_jiffies, 22);
		putchar('\n');
	}

	printf("\n/proc/schedstat (cumulative CPU runtime, field 7)\n");
	printf("%6s %22s\n", "CPU", "Runtime[ns]");
	for (int i = 0; i < count; i++) {
		int valid = snapshot->authoritative_runtime_valid &&
			    snapshot->authoritative_runtime_valid[i] &&
			    snapshot->authoritative_runtime_ns;

		print_raw_cpu_identity(i);
		putchar(' ');
		print_raw_unsigned(snapshot->authoritative_runtime_ns ?
				   snapshot->authoritative_runtime_ns[i] : 0, valid, 22);
		putchar('\n');
	}
}

static void print_raw_cpu_configuration(const struct sys_snapshot *snapshot)
{
	const struct cpu_freq_info *freqs = sys_snapshot_get_freqs(snapshot);
	int count = snapshot->effective_cpu_count;

	printf("\nCPU identity (/sys/devices/system/cpu/cpuN/topology and node membership)\n");
	printf("%6s %7s %7s %7s\n", "CPU", "Package", "Core", "Node");
	for (int i = 0; i < count; i++) {
		const struct cpu_desc *cpu = cpu_catalog_get_by_tracked_idx(i);

		print_raw_cpu_identity(i);
		putchar(' ');
		print_raw_signed(cpu ? cpu->package_id : -1, cpu && cpu->package_id >= 0, 7);
		putchar(' ');
		print_raw_signed(cpu ? cpu->core_id : -1, cpu && cpu->core_id >= 0, 7);
		putchar(' ');
		print_raw_signed(cpu ? cpu->numa_node : -1, cpu && cpu->numa_node >= 0, 7);
		putchar('\n');
	}

	printf("\nCPU frequency (/sys/devices/system/cpu/cpuN/cpufreq)\n");
	printf("  Current: cpuinfo_cur_freq; Min/Max: scaling_min_freq/scaling_max_freq.\n");
	printf("  Governor: scaling_governor; Boost: cpuN/cpufreq/boost or cpufreq/boost.\n");
	printf("  Min/Max/Governor/Boost are cached configuration, not fresh counter reads.\n");
	printf("%6s %12s %12s %12s %7s %s\n",
	       "CPU", "Current[kHz]", "Min[kHz]", "Max[kHz]", "Boost", "Governor");
	for (int i = 0; i < count; i++) {
		const struct cpu_freq_info *freq = freqs ? &freqs[i] : NULL;

		print_raw_cpu_identity(i);
		putchar(' ');
		print_raw_unsigned(freq ? freq->cur_freq : 0,
				   freq && freq->cur_freq_valid, 12);
		putchar(' ');
		print_raw_unsigned(freq ? freq->min_freq : 0,
				   freq && freq->min_freq_valid, 12);
		putchar(' ');
		print_raw_unsigned(freq ? freq->max_freq : 0,
				   freq && freq->max_freq_valid, 12);
		putchar(' ');
		print_raw_signed(freq ? freq->boost : -1,
				 freq && (freq->boost == 0 || freq->boost == 1), 7);
		printf(" %.*s\n", freq ? (int)sizeof(freq->governor) : 1,
		       raw_string(freq ? freq->governor : NULL));
	}
}

static void print_raw_cpuidle(const struct sys_snapshot *snapshot)
{
	int count = snapshot->effective_cpu_count;
	int states = snapshot->idle_state_count;

	printf("\nCPU idle states (/sys/devices/system/cpu/cpuN/cpuidle/stateN)\n");
	printf("  Time: time[us]; Usage: usage[count]; both are cumulative.\n");
	printf("  DisableCached is the cached disable bit; its read validity is not exposed.\n");
	printf("  Names are discovered state-index labels; availability is per CPU.\n");
	if (!states) {
		printf("  states: -\n");
		return;
	}
	printf("%6s %5s %22s %22s %9s %13s %s\n",
	       "CPU", "State", "Time[us]", "Usage[count]", "Available",
	       "DisableCached", "Name");
	for (int i = 0; i < count; i++) {
		const struct idle_state *idle = snapshot->idle ? snapshot->idle[i] : NULL;

		for (int state_idx = 0; state_idx < states; state_idx++) {
			const struct idle_state *state = idle ? &idle[state_idx] : NULL;
			int available = state && state->available;

			print_raw_cpu_identity(i);
			printf(" %5d ", state_idx);
			print_raw_unsigned(state ? state->time : 0,
					   available && state->time_valid, 22);
			putchar(' ');
			print_raw_unsigned(state ? state->usage : 0,
					   available && state->usage_valid, 22);
			putchar(' ');
			print_raw_signed(available, state != NULL, 9);
			putchar(' ');
			print_raw_signed(state ? state->disabled : 0, available, 13);
			printf(" %s\n", raw_string(get_idle_state_name(state_idx)));
		}
	}
}

static void print_raw_sensors(const struct sys_snapshot *snapshot)
{
	const char *device = get_uncore_freq_device_name();
	const char *temp_policy = get_summary_temp_policy_name();
	int count = snapshot->numa_temp_count;

	printf("\nMemory read counter (cumulative bytes, not bandwidth)\n");
	printf("  source: %s\n  memory_read[bytes]: ", raw_string(get_mem_bw_source_path()));
	print_raw_unsigned(snapshot->counters.mem_bw_counter,
			   snapshot->counters.mem_bw_valid, 0);
	printf("\n\nCurrent sensors (collector units)\n");
	printf("  power_source: %s\n  package_power[mW]: ",
	       raw_string(get_package_power_source_path()));
	print_raw_signed(snapshot->package_power_mw, snapshot->package_power_valid, 0);
	printf("\n  Power is normalized from the hwmon source unit to mW.\n");
	if (device && *device)
		printf("  uncore_source: /sys/class/devfreq/%s/cur_freq\n", device);
	else
		printf("  uncore_source: -\n");
	printf("  uncore_frequency[Hz]: ");
	print_raw_unsigned(snapshot->uncore_freq_hz, snapshot->uncore_freq_valid, 0);
	printf("\n  temperature_policy: %s\n", raw_string(temp_policy));
	if (!count) {
		printf("  temperature[mC]: -\n");
		return;
	}
	printf("%6s %16s %s\n", "Node", "Temperature[mC]", "Source");
	for (int node = 0; node < count; node++) {
		printf("%6d ", node);
		print_raw_signed(snapshot->numa_temps[node],
				 snapshot->numa_temp_valid_mask & (1U << node), 16);
		if (temp_policy && !strcmp(temp_policy, "thermal-zone-index"))
			printf(" /sys/class/thermal/thermal_zone%d/temp\n", node);
		else
			printf(" -\n");
	}
}

int serialize_raw_snapshot(const struct sys_snapshot *snapshot)
{
	if (!snapshot || snapshot->effective_cpu_count < 0 ||
	    snapshot->effective_cpu_count > MAX_CPUS ||
	    snapshot->idle_state_count < 0 || snapshot->idle_state_count > MAX_IDLE_STATES ||
	    snapshot->numa_temp_count < 0 || snapshot->numa_temp_count > 16) {
		errno = EINVAL;
		return -1;
	}

	print_raw_metadata(snapshot);
	print_raw_procstat(snapshot);
	print_raw_cpu_configuration(snapshot);
	print_raw_cpuidle(snapshot);
	print_raw_sensors(snapshot);
	return fflush(stdout) == EOF || ferror(stdout) ? -1 : 0;
}
