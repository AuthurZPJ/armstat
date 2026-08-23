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
#include <limits.h>
#include <sys/resource.h>

#include "armstat_cli.h"
#include "collector.h"
#include "aggregator.h"
#include "formatter.h"
#include "formatter_section.h"
#include "topology.h"
#include "power.h"
#include "pmu.h"
#include "cpuidle.h"
#include "cpu_inventory.h"
#include "sysstat.h"
#include "idle_backend.h"
#include "sampling_deadline.h"

#define PROBE_SCHEMA_VERSION 1

/* ============================================================================
 * MODULE INITIALIZATION
 * ============================================================================ */

static volatile sig_atomic_t done = 0;

static void signal_handler(int sig)
{
	(void)sig;
	done = 1;
}

static int ignore_sigpipe(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGPIPE, &sa, NULL) < 0) {
		fprintf(stderr, "Error: failed to ignore SIGPIPE: %s\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int open_output_stream(const char *output_file)
{
	int saved_errno;

	if (!output_file)
		return 0;

	if (!freopen(output_file, "w", stdout)) {
		saved_errno = errno;
		fprintf(stderr, "Error: cannot open output file %s: %s\n",
			output_file, strerror(saved_errno));
		return -1;
	}

	return 0;
}

static int finalize_stdout_output(void)
{
	if (fflush(stdout) == EOF || ferror(stdout)) {
		fprintf(stderr, "Error: failed to finalize output: %s\n",
			strerror(errno ? errno : EIO));
		return -1;
	}
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

static void print_probe_idle_state_names(void)
{
	int state_count = get_global_idle_state_count();

	if (state_count > MAX_VISIBLE_IDLE_STATES)
		state_count = MAX_VISIBLE_IDLE_STATES;
	for (int state = 0; state < state_count; state++) {
		const char *name = get_idle_state_name(state);

		if (name && *name)
			printf("  idle_state_%d_name: %s\n", state, name);
		else
			printf("  idle_state_%d_name: LPI-%d\n", state, state);
	}
}

static int run_probe(struct armstat_options *opts)
{
	int pmu_available = 0;
	int status = -1;
	long long package_power_mw;
	const char *idle_backend_name;

	if (init_collector() < 0) {
		fprintf(stderr, "Error: Failed to init collector for probe\n");
		fprintf(stderr, "  armstat requires ARM64 Linux with sysfs/procfs access.\n");
		return -1;
	}

	if (init_topology() < 0)
		fprintf(stderr, "Warning: Failed to init topology for probe\n");

	idle_backend_name = probe_idle_backend_name();

	if (probe_pmu_event("cycles") == 0) {
		pmu_available = 1;
	}

	/* Do not truncate an existing output until platform probing can run. */
	if (open_output_stream(opts->output_file) < 0)
		goto out;

	printf("armstat probe\n");
	printf("  probe_schema_version: %d\n", PROBE_SCHEMA_VERSION);
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
	print_probe_idle_state_names();
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
	if (read_total_power_mw(&package_power_mw) == 0)
		printf("  package_power_mw: %lld\n", package_power_mw);
	else
		printf("  package_power_mw: unavailable\n");
	if (get_package_power_source_path())
		printf("  package_power_source: %s\n",
		       get_package_power_source_path());
	printf("  package_power_candidates: %d\n",
	       get_package_power_candidate_count());
	if (get_package_power_candidate_count() > 1)
		printf("  package_power_note: ambiguous; expected exactly one "
		       "power_meter/power1_average source\n");
	printf("  numa_temp_sensors: %d\n", get_temp_numa_sensor_count());
	printf("  numa_temp_mask: 0x%08x\n", get_temp_numa_mask());
	printf("  summary_temp_policy: %s\n", get_summary_temp_policy_name());
	printf("  per_core_power: %s\n",
	       probe_yes_no(get_per_core_power_support()));
	printf("  mem_bw_supported: %s\n",
	       probe_yes_no(get_mem_bw_support()));
	if (get_mem_bw_source_path())
		printf("  mem_bw_source: %s\n", get_mem_bw_source_path());
	printf("  mem_bw_candidates: %d\n", get_mem_bw_candidate_count());
	if (get_mem_bw_candidate_count() > 1)
		printf("  mem_bw_note: ambiguous; expected exactly one "
		       "mem_bytes_read source\n");
	printf("  pmu_cycles: %s\n", probe_yes_no(pmu_available));
	if (!pmu_available)
		printf("  pmu_note: unavailable; check perf permissions, kernel PMU "
		       "support, and perf_event_open(2)\n");
	if (finalize_stdout_output() < 0)
		goto out;
	status = 0;

out:
	close_topology();
	close_pmu_events();
	close_sysstat_fds();
	cleanup_collector();
	return status;
}

/*
 * Initialize all subsystems
 */
static int init_modules(struct armstat_options *opts, struct sys_snapshot *snapshot)
{
	int original_priority;

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
	if (opts->pmu_events && (is_pmu_enabled() || is_ipc_enabled())) {
		if (opts->debug) fprintf(stderr, "Initializing PMU: %s\n", opts->pmu_events);
		if (init_pmu_events(opts->pmu_events) < 0) {
			fprintf(stderr, "Warning: PMU init failed; PMU columns will be shown as unavailable\n");
			fprintf(stderr, "  Events: %s\n", opts->pmu_events);
			fprintf(stderr, "  Note: check perf permissions, kernel support, or event availability\n");
		} else {
			if (opts->debug) fprintf(stderr, "PMU initialized successfully\n");
		}
	}

	if (!section_has_output()) {
		fprintf(stderr,
			"Error: selected columns cannot produce data in this mode/platform\n"
			"  Use --list for field scopes and --probe for platform capabilities.\n");
		return -1;
	}

	/* Setup signal handlers with sigaction for consistent SA_RESTART */
	{
		struct sigaction sa;
		sa.sa_handler = signal_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESTART;
		if (sigaction(SIGINT, &sa, NULL) < 0 ||
		    sigaction(SIGTERM, &sa, NULL) < 0) {
			fprintf(stderr, "Error: failed to install signal handlers: %s\n",
				strerror(errno));
			return -1;
		}
	}

	/*
	 * Like turbostat, best-effort elevate our own priority for interval
	 * mode. Ignore failure; lack of privilege is common and not fatal.
	 */
	#define SAMPLING_NICE_VALUE (-20)  /* highest normal nice priority */

	original_priority = boost_sampling_priority(SAMPLING_NICE_VALUE);
	if (opts->debug) {
		if (original_priority < -20)
			fprintf(stderr, "Priority boost unavailable; continuing at current nice\n");
		else
			fprintf(stderr, "Sampling priority boosted: nice %d -> -20\n",
				original_priority);
	}

	/* Phase 1: Establish baseline */
	if (collect_snapshot(snapshot) < 0) {
		fprintf(stderr, "Error: failed to collect baseline snapshot\n");
		return -1;
	}

	/* Allocate per-CPU record storage only when this mode can emit CPU rows. */
	setup_formatter_pool(!section_is_summary_mode() && section_emit_cpu() ?
			     sys_snapshot_get_effective_cpu_count(snapshot) : 0);

	/*
	 * Phase 1b: Warm up aggregator with baseline for correct first interval delta
	 * This sets prev_counters so the FIRST real interval has correct deltas
	 * (CtxSw, IRQs, PMU, etc.)
	 */
	{
		struct interval_stats baseline_stats;
		calculate_interval_stats(snapshot, &baseline_stats);
	}

	/* Preserve an existing output file unless initialization fully succeeds. */
	if (open_output_stream(opts->output_file) < 0)
		return -1;

	/* Print interval header only after stdout is on its final destination. */
	print_interval_header(opts, opts->interval);

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
 * Returns 1 if the caller should abort (done flag set), 0 if sleep completed,
 * or -1 on an unrecoverable sleep error.
 */
static int safe_interval_sleep(const struct timespec *ts)
{
	struct timespec remaining = *ts;

	while (!done && nanosleep(&remaining, &remaining) == -1) {
		if (errno != EINTR) {
			fprintf(stderr, "Error: interval sleep failed: %s\n",
				strerror(errno));
			return -1;
		}
	}
	return done ? 1 : 0;
}

static int monotonic_now_ns(unsigned long long *now_ns)
{
	struct timespec ts;

	if (!now_ns)
		return -1;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
		fprintf(stderr, "Error: clock_gettime(CLOCK_MONOTONIC) failed: %s\n",
			strerror(errno));
		return -1;
	}
	*now_ns = (unsigned long long)ts.tv_sec * 1000000000ULL +
		  (unsigned long long)ts.tv_nsec;
	return 0;
}

static int safe_sleep_until(unsigned long long deadline_ns)
{
	unsigned long long now_ns;
	unsigned long long remaining_ns;
	struct timespec remaining;

	if (monotonic_now_ns(&now_ns) < 0)
		return -1;
	if (now_ns >= deadline_ns)
		return done ? 1 : 0;

	remaining_ns = deadline_ns - now_ns;
	remaining.tv_sec = (time_t)(remaining_ns / 1000000000ULL);
	remaining.tv_nsec = (long)(remaining_ns % 1000000000ULL);
	return safe_interval_sleep(&remaining);
}

static int advance_sampling_deadline(unsigned long long *deadline_ns,
				     unsigned long long interval_ns)
{
	unsigned long long now_ns;

	if (monotonic_now_ns(&now_ns) < 0)
		return -1;
	if (sampling_deadline_advance(deadline_ns, interval_ns, now_ns) < 0) {
		fprintf(stderr, "Error: sampling deadline overflow\n");
		return -1;
	}
	return 0;
}

static int run_loop(struct armstat_options *opts, struct sys_snapshot *snapshot)
{
	struct interval_stats stats;
	struct interval_record *rec;
	unsigned long long interval_ns;
	unsigned long long next_deadline_ns;
	unsigned long long iteration = 1;
	int sleep_result;
	int status = 0;

	/* -D overrides -n: always dump exactly one interval */
	if (opts->dump_once)
		opts->iterations = 1;

	interval_ns = (unsigned long long)(opts->interval * 1000000000.0 + 0.5);
	if (interval_ns == 0)
		interval_ns = 1;
	if (sampling_deadline_init(snapshot->sample_monotonic_ns, interval_ns,
				   &next_deadline_ns) < 0) {
		fprintf(stderr, "Error: sampling deadline overflow\n");
		return -1;
	}

	/* Wait for the first deadline measured from the baseline snapshot. */
	sleep_result = safe_sleep_until(next_deadline_ns);
	if (sleep_result != 0) {
		if (sleep_result < 0)
			status = -1;
		goto out;
	}

	while (!done) {
		/* Collect raw data */
		if (collect_snapshot(snapshot) < 0) {
			fprintf(stderr, "Error: sampling stopped after collector failure\n");
			status = -1;
			break;
		}

		/*
		 * A zero-length interval means we are still on the initial baseline
		 * sample or just rebuilt runtime state after CPU topology change.
		 * Do not render it as a normal interval, otherwise users see
		 * misleading rows such as Idle%=0 / Busy%=100 for what is really an
		 * incomplete baseline.
		 */
		if (sys_snapshot_get_interval_delta_us(snapshot) == 0) {
			/* Consume the rebuilt snapshot as the new internal baseline. */
			calculate_interval_stats(snapshot, &stats);
			next_deadline_ns = snapshot->sample_monotonic_ns;
			if (advance_sampling_deadline(&next_deadline_ns,
						      interval_ns) < 0) {
				status = -1;
				break;
			}
			sleep_result = safe_sleep_until(next_deadline_ns);
			if (sleep_result != 0) {
				if (sleep_result < 0)
					status = -1;
				break;
			}
			continue;
		}

		/* Calculate interval statistics */
		calculate_interval_stats(snapshot, &stats);

		/* Output — dispatch directly to the serializer for this format */
		rec = build_interval_record(snapshot, &stats, iteration);
		if (!rec) {
			fprintf(stderr, "Error: failed to build interval output record\n");
			status = -1;
			break;
		}

		switch (opts->format) {
		case FORMAT_JSON:
			serialize_json(rec);
			break;
		case FORMAT_CSV:
			serialize_csv(rec);
			break;
		default:
			serialize_text(rec);
			break;
		}
		free_interval_record(rec);

		if (fflush(stdout) == EOF || ferror(stdout)) {
			status = -1;
			break;
		}

		/* Check exit condition */
		if (opts->iterations > 0 &&
		    iteration >= (unsigned int)opts->iterations)
			break;
		if (iteration == ULLONG_MAX) {
			fprintf(stderr, "Error: interval sequence exhausted\n");
			status = -1;
			break;
		}

		/* Hold an absolute cadence instead of accumulating output overhead. */
		if (advance_sampling_deadline(&next_deadline_ns, interval_ns) < 0) {
			status = -1;
			break;
		}
		sleep_result = safe_sleep_until(next_deadline_ns);
		if (sleep_result != 0) {
			if (sleep_result < 0)
				status = -1;
			break;
		}
		iteration++;
	}

out:
	/* Close output format (close the JSON array) */
	if (opts->format == FORMAT_JSON)
		close_machine_json();

	if (finalize_stdout_output() < 0)
		status = -1;

	return status;
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
	int run_result;

	/* Turn a closed output pipe into a checked EPIPE and orderly cleanup. */
	if (ignore_sigpipe() < 0)
		return 1;

	/* Parse command line */
	parse_result = parse_args(argc, argv, &opts);
	if (parse_result) {
		if (parse_result < 0)
			return 1;
		return finalize_stdout_output() < 0 ? 1 : 0;
	}

	/* Listing static metadata does not probe the host platform. */
	if (opts.list_counters) {
		list_counters();
		return finalize_stdout_output() < 0 ? 1 : 0;
	}

#if !defined(__aarch64__) || !defined(__linux__)
	fprintf(stderr,
		"Warning: armstat targets ARM64 Linux; most telemetry sources "
		"will be unavailable on other platforms.\n");
#endif

	if (apply_default_pmu_events(&opts) < 0)
		return 1;

	if (opts.probe_only)
		return run_probe(&opts) < 0 ? 1 : 0;

	/* Initialize all modules */
	if (init_modules(&opts, &snapshot) < 0) {
		cleanup_modules(&opts);
		return 1;
	}

	/* Run main sampling loop */
	run_result = run_loop(&opts, &snapshot);

	/* Cleanup */
	cleanup_modules(&opts);

	return run_result < 0 ? 1 : 0;
}
