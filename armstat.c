/* SPDX-License-Identifier: GPL-2.0 */
/*
 * armstat.c - ARM Server monitoring tool: main loop and module lifecycle
 *
 * Delegates CLI parsing to armstat_cli.c.  This file is responsible for:
 *   - module initialization / cleanup ordering
 *   - the sampling interval loop
 *   - signal handling and priority boost
 *   - the --probe one-shot capability dump
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/resource.h>

#include "armstat_cli.h"
#include "collector.h"
#include "aggregator.h"
#include "formatter.h"
#include "topology.h"
#include "power.h"
#include "pmu.h"
#include "cpuidle.h"
#include "cpu_inventory.h"
#include "sysstat.h"
#include "idle_backend.h"

/* ============================================================================
 * MODULE INITIALIZATION
 * ============================================================================ */

static volatile sig_atomic_t done = 0;

static void signal_handler(int sig)
{
	(void)sig;
	done = 1;
}

static int open_output_stream(const char *output_file)
{
	if (!output_file)
		return 0;

	FILE *outfp = fopen(output_file, "w");
	if (!outfp) {
		fprintf(stderr, "Cannot open %s: %s\n",
			output_file, strerror(errno));
		return -1;
	}

	if (dup2(fileno(outfp), fileno(stdout)) < 0) {
		fprintf(stderr, "Cannot redirect stdout to %s: %s\n",
			output_file, strerror(errno));
		fclose(outfp);
		return -1;
	}
	fclose(outfp);
	return 0;
}

static const char *probe_yes_no(int value)
{
	return value ? "yes" : "no";
}

/*
 * Best-effort priority boost for interval sampling.
 *
 * Match turbostat's intent: try to reduce self-induced scheduling jitter by
 * raising the process priority, but never fail armstat if permission is
 * missing. On Linux this generally requires elevated privileges to reach -20.
 *
 * Returns the previous nice value on success, or a value < -20 on failure.
 */
static int boost_sampling_priority(int target_nice)
{
	int original_priority;
	int current_priority;

	errno = 0;
	original_priority = getpriority(PRIO_PROCESS, 0);
	if (errno && original_priority == -1)
		return -21;

	if (setpriority(PRIO_PROCESS, 0, target_nice) != 0)
		return -21;

	errno = 0;
	current_priority = getpriority(PRIO_PROCESS, 0);
	if (errno && current_priority == -1)
		return -21;
	if (current_priority != target_nice)
		return -21;

	return original_priority;
}

static const char *probe_idle_backend_name(void)
{
	if (is_cpuidle_enabled() && get_global_idle_state_count() > 0)
		return "busy:procstat/schedstat + cpuidle(LPI)";

	return "busy:procstat/schedstat";
}

static int run_probe(struct armstat_options *opts)
{
	int pmu_available = 0;
	const char *idle_backend_name;

	if (open_output_stream(opts->output_file) < 0)
		return -1;

	if (init_collector() < 0) {
		fprintf(stderr, "Error: Failed to init collector for probe\n");
		fprintf(stderr, "  armstat requires ARM64 Linux with sysfs/procfs access.\n");
		return -1;
	}

	if (init_topology() < 0)
		fprintf(stderr, "Warning: Failed to init topology for probe\n");

	idle_backend_name = probe_idle_backend_name();

	if (init_pmu_events("cycles") == 0) {
		pmu_available = 1;
	}
	close_pmu_events();

	printf("armstat probe\n");
	printf("  online_cpus: %d\n", cpu_catalog_online_count());
	printf("  tracked_cpus: %d\n", cpu_catalog_tracked_count());
	printf("  sockets: %d\n", get_socket_count());
	printf("  cores_per_socket: %d\n", get_cores_per_socket());
	printf("  cpus_per_core: %d\n", get_cpus_per_core());
	printf("  numa_nodes: %d\n", get_numa_node_count());
	printf("  idle_backend: %s\n", idle_backend_name);
	printf("  busy_source_requested: %s\n", get_busy_source_mode_name());
	printf("  busy_source_effective: %s\n", get_busy_source_effective_name());
	printf("  nohz_full_cpus: %d\n", get_nohz_full_cpu_count());
	printf("  idle_states: %d\n", get_global_idle_state_count());
	printf("  uncore_freq_supported: %s\n",
	       probe_yes_no(has_uncore_freq_support()));
	if (has_uncore_freq_support()) {
		unsigned long long uncore_freq_hz = 0;
		const char *uncore_device = get_uncore_freq_device_name();

		if (read_uncore_freq(&uncore_freq_hz) == 0) {
			printf("  uncore_freq_device: %s\n",
			       uncore_device ? uncore_device : "unknown");
			printf("  uncore_freq_mhz: %.2f\n",
			       uncore_freq_hz / 1000000.0);
		}
	}
	printf("  package_power_mw: %lld\n", get_total_power());
	printf("  numa_temp_sensors: %d\n", get_temp_numa_count());
	printf("  summary_temp_policy: %s\n", get_summary_temp_policy_name());
	printf("  per_core_power: %s\n",
	       probe_yes_no(get_per_core_power_support()));
	printf("  mem_bw_supported: %s\n",
	       probe_yes_no(get_mem_bw_support()));
	printf("  pmu_cycles: %s\n", probe_yes_no(pmu_available));
	if (!pmu_available)
		printf("  pmu_note: requires root or permissive perf_event_paranoid; "
		       "see perf_event_open(2)\n");

	close_topology();
	close_pmu_events();
	close_sysstat_fds();
	cleanup_collector();
	return 0;
}

/*
 * Initialize all subsystems
 */
static int init_modules(struct armstat_options *opts, struct sys_snapshot *snapshot)
{
	int original_priority;

	if (open_output_stream(opts->output_file) < 0)
		return -1;

	/* Initialize subsystems */
	if (opts->debug) fprintf(stderr, "Initializing collector...\n");
	if (init_collector() < 0) {
		fprintf(stderr, "Error: Failed to init collector\n");
		fprintf(stderr, "  armstat requires ARM64 Linux with sysfs/procfs access.\n");
		return -1;
	}

	if (get_busy_source_mode() == BUSY_SOURCE_TASK_CLOCK) {
		fprintf(stderr,
			"Warning: --busy-source task-clock now uses /proc/schedstat runtime\n"
			"  as a compatibility replacement for unreliable CPU-wide perf task-clock\n");
	} else if (opts->debug && get_busy_source_mode() == BUSY_SOURCE_AUTO) {
		fprintf(stderr,
			"Debug: auto busy-source uses /proc/stat on ordinary CPUs\n"
			"  and /proc/schedstat on nohz_full CPUs when available\n");
	}

	if (opts->debug) fprintf(stderr, "Initializing topology...\n");
	if (init_topology() < 0)
		fprintf(stderr, "Warning: Failed to init topology\n");

	if (opts->debug) fprintf(stderr, "Initializing aggregator...\n");
	if (init_aggregator() < 0) {
		fprintf(stderr, "Error: Failed to init aggregator\n");
		return -1;
	}

	if (opts->debug) fprintf(stderr, "Initializing formatter...\n");
	update_idle_state_visibility();
	update_temp_field_visibility();

	/* Initialize PMU if requested */
	if (opts->pmu_events) {
		if (opts->debug) fprintf(stderr, "Initializing PMU: %s\n", opts->pmu_events);
		if (init_pmu_events(opts->pmu_events) < 0) {
			fprintf(stderr, "Warning: PMU init failed; PMU columns will be shown as unavailable\n");
			fprintf(stderr, "  Events: %s\n", opts->pmu_events);
			fprintf(stderr, "  Note: check perf permissions, kernel support, or event availability\n");
		} else {
			if (opts->debug) fprintf(stderr, "PMU initialized successfully\n");
		}
	}

	/* Setup signal handlers with sigaction for consistent SA_RESTART */
	{
		struct sigaction sa;
		sa.sa_handler = signal_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESTART;
		sigaction(SIGINT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
	}

	/*
	 * Like turbostat, best-effort elevate our own priority for interval
	 * mode. Ignore failure; lack of privilege is common and not fatal.
	 */
	#define SAMPLING_NICE_VALUE (-20)  /* highest real-time priority, matching turbostat */

	original_priority = boost_sampling_priority(SAMPLING_NICE_VALUE);
	if (opts->debug) {
		if (original_priority < -20)
			fprintf(stderr, "Priority boost unavailable; continuing at current nice\n");
		else
			fprintf(stderr, "Sampling priority boosted: nice %d -> -20\n",
				original_priority);
	}

	/* Print interval header */
	print_interval_header(opts, opts->interval);

	/* Phase 1: Establish baseline */
	collect_snapshot(snapshot);

	/* Setup formatter pool after we know the CPU count */
	setup_formatter_pool(snapshot->effective_cpu_count);

	/*
	 * Phase 1b: Warm up aggregator with baseline for correct first interval delta
	 * This sets prev_counters so the FIRST real interval has correct deltas
	 * (CtxSw, IRQs, PMU, etc.)
	 */
	{
		struct interval_stats baseline_stats;
		calculate_interval_stats(snapshot, &baseline_stats);
	}

	return 0;
}

/* ============================================================================
 * MAIN LOOP
 * ============================================================================ */

/*
 * Run the main sampling loop
 */

/*
 * Sleep for the requested interval, retrying on signal interruption.
 * Returns 1 if the caller should abort (done flag set), 0 if sleep completed.
 */
static int safe_interval_sleep(const struct timespec *ts)
{
	struct timespec remaining = *ts;

	while (!done && nanosleep(&remaining, &remaining) == -1) {
		if (errno != EINTR)
			return 0;
	}
	return done ? 1 : 0;
}

static void run_loop(struct armstat_options *opts, struct sys_snapshot *snapshot)
{
	struct interval_stats stats;
	struct timespec ts;
	struct interval_record *rec;
	int iteration = 1;

	/* -D overrides -n: always dump exactly one interval */
	if (opts->dump_once)
		opts->iterations = 1;

	/* Wait for first interval */
	ts.tv_sec = (time_t)opts->interval;
	ts.tv_nsec = (long)((opts->interval - (double)ts.tv_sec) * 1000000000.0 + 0.5);
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	if (safe_interval_sleep(&ts)) return;

	while (!done) {
		/* Collect raw data */
		collect_snapshot(snapshot);

		/*
		 * A zero-length interval means we are still on the initial baseline
		 * sample or just rebuilt runtime state after CPU topology change.
		 * Do not render it as a normal interval, otherwise users see
		 * misleading rows such as Idle%=0 / Busy%=100 for what is really an
		 * incomplete baseline.
		 */
		if (snapshot->interval_delta_us == 0) {
			if (safe_interval_sleep(&ts)) break;
			continue;
		}

		/* Calculate interval statistics */
		calculate_interval_stats(snapshot, &stats);

		/* Output — dispatch directly to the serializer for this format */
		rec = build_interval_record(snapshot, &stats, iteration);
		if (rec) {
			switch (opts->format) {
			case FORMAT_JSON:
				serialize_json(rec, iteration);
				break;
			case FORMAT_CSV:
				serialize_csv(rec);
				break;
			default:
				serialize_text(rec, iteration);
				break;
			}
			free_interval_record(rec);
		}

		/* Check exit condition */
		if (opts->iterations > 0 && iteration >= opts->iterations)
			break;

		/* Sleep before next sample */
		if (safe_interval_sleep(&ts)) break;
		iteration++;
	}

	/* Close output format (close the JSON array) */
	if (opts->format == FORMAT_JSON)
		close_machine_json();
}

/* ============================================================================
 * CLEANUP
 * ============================================================================ */

/*
 * Cleanup all subsystems
 */
static void cleanup_modules(struct armstat_options *opts)
{
	(void)opts;

	close_pmu_events();
	close_sysstat_fds();
	close_topology();
	cleanup_collector();
	cleanup_aggregator();
	cleanup_formatter_pool();
}

/* ============================================================================
 * MAIN ENTRY POINT
 * ============================================================================ */

int main(int argc, char *argv[])
{
	struct armstat_options opts = default_options;
	struct sys_snapshot snapshot;
	int parse_result;

	/* Parse command line */
	parse_result = parse_args(argc, argv, &opts);
	if (parse_result)
		return parse_result < 0 ? 1 : 0;  /* Exited early */

#if !defined(__aarch64__) || !defined(__linux__)
	fprintf(stderr,
		"Warning: armstat targets ARM64 Linux; most telemetry sources "
		"will be unavailable on other platforms.\n");
#endif

	apply_default_pmu_events(&opts);

	/* List counters and exit */
	if (opts.list_counters) {
		list_counters();
		return 0;
	}

	if (opts.probe_only)
		return run_probe(&opts) < 0 ? 1 : 0;

	/* Initialize all modules */
	if (init_modules(&opts, &snapshot) < 0) {
		cleanup_modules(&opts);
		return 1;
	}

	/* Run main sampling loop */
	run_loop(&opts, &snapshot);

	/* Cleanup */
	cleanup_modules(&opts);

	return 0;
}
