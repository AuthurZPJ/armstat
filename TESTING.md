# armstat Testing Guide

<p align="center">
  <a href="README.md">← Back to README</a> |
  <a href="DESIGN.md">Design</a> |
  <a href="EXPORTS.md">Exports</a>
</p>

This document describes the current testing workflow for `armstat`.

## Goals

The current test strategy is split into two layers:

- quick local regression checks that should run on any development machine
- target-machine validation on an ARM server with the expected telemetry

The first layer protects code structure and machine-readable export contracts.
The second layer validates platform-dependent behavior such as cpuidle, PMU,
NUMA temperature mapping, and package power.

The repository CI workflow (`.github/workflows/ci.yml`) runs on an Ubuntu 24.04
ARM64 runner for pushes and pull requests. It promotes GCC warnings to errors,
including format, undefined-macro, shadowing, and missing/strict-prototype
checks; runs GCC's static analyzer and the AddressSanitizer/UBSan suite;
returns to the default fortified release flags for a final test pass; and
finishes with `make target-test` runtime acceptance plus a 1,000-interval
short-period resource-stability soak. Hardware-specific PMU and sensor
semantics still require the real-server layer below.

## 1. Local Build Check

From the repository root:

```bash
make clean
make
```

This confirms:

- the C sources still build cleanly with format, undefined-macro, shadowing,
  and strict/missing-prototype warnings enabled
- the formatter/export path still links correctly
- the helper scripts remain compatible with the current source tree

For memory/undefined-behavior debugging, there is also a sanitizer build:

```bash
make debug-test
```

That target rebuilds the binary and all C tests with AddressSanitizer and UBSan
enabled, then runs the complete suite. Use `make debug` when only the
instrumented binary is needed.

On Linux with GCC, run the compiler's path-sensitive static analyzer with:

```bash
make analyze
```

The target treats analyzer findings as errors. It suppresses only the file and
allocation leak diagnostics caused by armstat's intentional process-lifetime
`/proc` stream cache, which is closed centrally during shutdown.
Analyzer objects and its binary remain under `.armstat-analysis/`, so this
check does not replace a release binary that is ready to install or test.

An out-of-tree release build is supported and must keep all generated files in
the selected output directory:

```bash
make O=/tmp/armstat-build
/tmp/armstat-build/armstat --version
```

The build records compiler identity, compiler target, and flag settings in the
output directory. Changing the compiler/version/target, `CFLAGS`, `LDFLAGS`, or
`LDLIBS` invalidates incompatible objects instead of silently reusing them.
Each release/custom/release transition uses a parallel build, and the regression
compares binary checksums, covering coarse filesystem timestamps as well as
normal incremental builds.
The build regression also checks that test executables are relinked across flag
changes, that the `VERSION` file reaches build metadata and `--version`, and on
Linux that the default executable has PIE, RELRO, and immediate binding.
It clears inherited compiler, linker, cross-build, and output-directory
overrides before establishing that release baseline, so invoking the parent
suite with sanitizer or coverage flags cannot weaken the check accidentally.

## 2. Smoke Tests

Run:

```bash
make test
```

This currently executes:

- `tests/test_core_logic`
- `tests/test_column_selection`
- `tests/test_runtime_smoke`
- `tests/test_cpu_inventory`
- `tests/test_section_policy`
- `tests/test_cli_smoke.sh`
- `tests/test_plot_loaders.py`
- `tests/test_csv_streaming.py`
- `tests/test_build.sh`

The smoke tests intentionally stay small and fast. They verify that:

- idle percentage, busy percentage, iowait-as-idle accounting, schedstat clamp,
  and procstat jiffy-conversion overflow handling are correct
- schedstat CPU records require nine fields and use documented field 7
- strict numeric and CPU-list parsers reject malformed, overflowing, reversed,
  empty, and truncated input while accepting long sparse lists and retaining
  the total count beyond the fixed CPU mask
- ARM PMUv3 built-in aliases resolve to the intended architectural event codes,
  duplicate PMU names and over-limit lists are rejected independently, and IPC
  remains unavailable unless both named inputs are present
- PMU initialization best-effort raises a constrained soft `RLIMIT_NOFILE` for
  the tracked CPU/event requirement without exceeding the hard limit
- transient procstat, power, memory-bandwidth, system-counter, and PMU failures
  emit unavailable values and re-establish cumulative baselines on recovery
- frequency, sparse/signed temperature, split-idle, and PMU validity reaches
  text/JSON/CSV output without fabricating zeroes
- package aggregation covers more than sixteen distinct package IDs
- online-mask matching checks actual CPU membership plus represented and full
  counts, so the hotplug fast path cannot miss a same-count replacement or a
  change beyond CPU ID 1023
- a hotplug rebuild refreshes the tracked cpufreq state and consumes a new
  baseline before interval output resumes
- exact `-s` / `-H` field selection does not accidentally enable or disable an
  entire column group
- repeated `-s` options form a union, and `-p` / `-I` / `-J` stay effective on
  either side of a show whitelist
- `-a` / `all` enables only the base column groups and does not implicitly
  enable PMU or IPC
- hiding `all` clears explicit PMU / IPC state as well
- CLI/runtime combinations such as `-S -a`, `-a -I`, `--probe`, and
  busy-source parsing stay wired correctly
- standard `-h` succeeds, static `--list` stays free of platform-probe noise,
  exposes registry-backed exact field IDs plus type/unit metadata, and help
  explains unlimited versus one-complete-interval execution
- field IDs and JSON keys remain unique within their serialization scope
- a failed platform probe does not truncate a pre-existing output file
- an invalid `ARMSTAT_TEMP_POLICY` is rejected without truncating a
  pre-existing probe output file
- probe output reports parseable package-power and memory-bandwidth candidate
  counts; ambiguous discovery is rejected instead of selecting a first match
- `--help`, `--version`, `--list`, and `--probe` return failure when Linux
  reports an output write error instead of silently reporting success
- a closed downstream pipe becomes a checked output failure with exit status
  1 instead of an abrupt `SIGPIPE` termination
- the startup banner renders 1 s, 10 ms, and 1 us intervals without rounding a
  valid subsecond setting to zero
- schema 7 exports carry positive measured `duration_us`, retain nanosecond
  timestamps, and use RFC 3339 offsets; plotting loaders prefer nanosecond
  time, so subsecond samples do not collapse onto one whole-second point
- target captures have strictly increasing sample timestamps while cadence is
  scheduled from monotonic deadlines rather than output-completion sleeps
- absolute-deadline arithmetic preserves phase before the next slot, skips one
  or many expired slots without catch-up sampling, and rejects overflow
- `-D` text remains self-describing by default, while `-D -q` is explicitly
  headerless
- JSON/CSV serializers still emit summary-only, package-only, CPU-only, and
  rectangular mixed `SUM`/`PKG`/`CPU` layouts end-to-end
- summary CSV uses a `Scope` header with `SUM` row values, package identity is
  serialized once, unavailable strings stay missing, JSON boost is boolean,
  and interval counts do not acquire misleading decimal suffixes
- exact package field selection enables package output instead of producing a
  header-only CSV
- empty or mode-incompatible effective selections are rejected after platform
  discovery and before an existing output file can be truncated
- successful `--output` replaces stale content with valid JSON, `--export`
  writes probe output, and an output path that is a directory fails clearly
- summary JSON exports can still be loaded by `scripts/plot_sum.py`
- summary CSV exports can still be loaded by `scripts/plot_sum.py`
- CPU JSON exports can still be loaded by `scripts/plot_cpu.py`
- CPU CSV exports can still be loaded by `scripts/plot_cpu.py`
- CSV sample-range loading stays streaming for 10,000 summary rows and 1,000
  samples across eight CPUs, while JSON retains its documented full-load model
- the current machine-readable export contract still matches
  `schema_version = 7`, while plotting loaders retain versions 4 through 6
  compatibility, resolve `freq` by scope, and ignore package rows for SUM/CPU
  plots
- an empty JSON stream is still a valid array
- out-of-tree build, compiler-flag transitions, staged install, staged
  uninstall, and installed license presence work end to end

These tests do **not** validate ARM runtime behavior.

## 3. Target ARM Server Validation

Start with the automated runtime acceptance on a real machine:

```bash
make target-test
```

It verifies probe schema/version and CPU counts, schema-7 JSON and CSV
captures, measured interval duration, RFC 3339 time, rectangular summary and
mixed-scope CSV rows, real `SUM`/`PKG`/`CPU` identities, exact package-only CSV
selection, identified sysfs sources for required power and memory-bandwidth
capabilities, unique-source candidate counts, regular `--output`/`--export`
file handling, and valid empty JSON output when interrupted before the first
interval. Set `ARMSTAT_TARGET_INTERVAL` and `ARMSTAT_TARGET_SAMPLES` to tune the
short capture.

By default, unavailable platform features are accepted as an explicit
degraded configuration so the test can run on generic ARM64 CI hosts. For a
real deployment candidate, set each capability promised by that platform to
`1`: `ARMSTAT_REQUIRE_PMU`, `ARMSTAT_REQUIRE_CPUIDLE`,
`ARMSTAT_REQUIRE_POWER`, `ARMSTAT_REQUIRE_TEMP`, `ARMSTAT_REQUIRE_MEMBW`, and
`ARMSTAT_REQUIRE_UNCORE`. A required capability must be reported by `--probe`
and must produce a finite value during normal JSON sampling; PMU additionally
requires positive `cycles` and `instructions` with a valid IPC sample. For
example:

```bash
sudo env ARMSTAT_REQUIRE_PMU=1 ARMSTAT_REQUIRE_CPUIDLE=1 \
  ARMSTAT_REQUIRE_POWER=1 ARMSTAT_REQUIRE_TEMP=1 \
  ARMSTAT_REQUIRE_MEMBW=1 make target-test
```

Enable only capabilities that the deployment hardware contract actually
promises. PMU validation normally needs root or a permissive
`perf_event_paranoid` setting. To include the 30-minute gate below in the same
run, use:

```bash
ARMSTAT_SOAK_ITERATIONS=1800 ARMSTAT_SOAK_INTERVAL=1 make target-test
```

The soak fails on malformed or incomplete JSON, more than 16 diagnostic lines,
resident memory above 256 MiB, or more than 256 open file descriptors. Bounded
startup diagnostics remain visible so expected capability degradation can be
reviewed. Override these ceilings for a documented deployment profile with
`ARMSTAT_MAX_DIAGNOSTIC_LINES`, `ARMSTAT_MAX_RSS_KIB`, and
`ARMSTAT_MAX_OPEN_FDS`.

The following commands remain the most useful manual checks on a real machine.

### Core output

```bash
./armstat -i 1 -n 2
./armstat -i 1 -n 2 -S
./armstat -i 1 -n 2 -S -a
```

Check:

- summary rows are printed correctly
- CPU rows are sorted by real CPU ID
- headers and values line up
- `Governor` / `Boost` / `Temp` / `LPI-*` are readable
- `--probe` online/tracked counts match the host; on systems beyond the
  `0..1023` representable ID range, a truncation warning is present

### Idle / busy semantics

```bash
./armstat -i 1 -n 3 -s cpu,LPI-0,LPI-1,Idle%,IOWait%,Busy%
./armstat -i 1 -n 3 -S --busy-source procstat
./armstat -i 1 -n 3 -S --busy-source schedstat
```

Check:

- `Idle% + Busy%` stays close to `100`
- `IOWait%` is reported separately but remains a subset of procstat idle
- visible `LPI-*` columns remain close to `Idle%`
- disabled/missing idle-state wakeup fields show unavailable rather than zero
- `schedstat` behaves sensibly on `nohz_full` CPUs
- after a transient source read failure, the affected fields show unavailable
  for the failure/recovery boundary rather than a 0/100% spike

### PMU / IPC

```bash
./armstat -i 1 -n 2 -I
./armstat -i 1 -n 2 -S -p cycles,instructions
```

Check:

- PMU columns appear when requested
- unsupported PMU data degrades cleanly instead of disappearing silently
- `-I` does not corrupt Busy/Idle values
- treat the `--probe` PMU result only as a `cycles` check on the first tracked
  CPU, then validate the requested event set through normal sampling
- compare cycles/instructions against `perf stat` under a steady pinned workload
- verify `mem-read` / `mem-write` behave as retired load/store event counts,
  not byte counters
- repeat with enough events to force multiplexing and confirm scaled rates stay
  plausible

### NUMA / temperature / power

```bash
./armstat -i 1 -n 2 -s cpu,pkg,core,node,temp
./armstat -i 1 -n 2 -S -s power,temp,energy
./armstat --probe
```

Check:

- CPU rows map to the correct NUMA node
- CPU temperatures match the expected `Temp0..` summary-temperature mapping
- sparse thermal-zone numbering does not shift `TempN` identities, and negative
  readings remain negative when the hardware exposes them
- package power appears only at `SUM` scope
- `--probe` reflects the actual platform sensor model, including
  `summary_temp_policy`, candidate counts, selected paths, and any ambiguity
  notes; a production metric must not depend on directory iteration order

### Export validation

```bash
./armstat -S -f json -O summary.json -n 2
./armstat -S -f csv -O summary.csv -n 2
./armstat -f json -O cpus.json -n 2
./armstat -f csv -O cpus.csv -n 2
```

Check:

- exports contain `schema_version`
- current exports report `schema_version = 7`
- JSON/CSV contains a positive measured `duration_us`
- CSV contains `timestamp`, `timestamp_ns`, and `timestamp_iso`
- `timestamp_ns` is consistent with `timestamp`, and the ISO value is valid
  RFC 3339 with fractional seconds and a colon-delimited UTC offset
- summary CSV uses `Scope` as the identity header and `SUM` as its value
- package JSON/CSV contains one package identity, not a duplicate data field
- JSON governor/boost values are string/boolean when available and `null`
  when unavailable
- JSON/CSV can be loaded by the helper scripts
- unavailable sensors are `null` in JSON and empty in CSV, while a real zero is
  still numeric zero
- JSON never emits `NaN` or infinity tokens

### Hotplug recovery

In a controlled maintenance window, run a multi-interval capture and offline,
then re-online, a non-boot CPU through the platform's normal administration
workflow. Restore the CPU even if the check fails.

Check:

- CPU membership changes are detected even if the total count later returns to
  the original value
- the rebuild boundary is consumed as a baseline and is not printed as a
  0/100% spike
- cpufreq, cpuidle, PMU, and topology state follow the new tracked CPU set
- JSON remains syntactically valid if the process receives `SIGINT` or
  `SIGTERM` before its first visible interval

### Plotting smoke checks

```bash
python3 scripts/plot_sum.py summary.json --preset power-temp
python3 scripts/plot_cpu.py cpus.json --preset busy --top 8
```

Check:

- helper scripts accept current exports without manual conversion
- plotting works with the installed `schema_version`

### Long-running stability

Run at least a 30-minute summary capture with the intended production interval:

```bash
./armstat -S -a -f json -O soak.json -i 1 -n 1800
```

The `ARMSTAT_SOAK_ITERATIONS=1800` target-test invocation above automates the
bounded-resource and export checks. During and after the run, also check:

- the process does not skip intervals or lose required telemetry unexpectedly
- interrupting a second run with `SIGINT` and `SIGTERM` closes the JSON array
  and releases PMU/sysfs descriptors

## 4. What Is Covered vs. Not Covered

Currently covered:

- in-tree/out-of-tree build, configuration invalidation, install, uninstall,
  and installed license presence
- strict input and kernel-text parsing
- injected read-failure, reset, unavailable-value, and recovery-baseline paths
- hotplug-dependent state rebuild with a synthetic CPU inventory
- helper script loader compatibility
- machine-readable export contract stability

Not yet covered by automated tests:

- cpuidle behavior on real hardware
- PMU availability and scaling correctness on specific ARM platforms
- hotplug behavior
- nohz_full behavior
- package power / temperature source discovery on real machines

Those still require target-machine validation.

## 5. Release Gate

Before tagging a release candidate, run from a clean tree:

```bash
make clean
make test
make debug-test
make clean
make test
```

The final `make test` confirms that returning from sanitizer flags to the
default fortified release flags also rebuilds correctly. Release sign-off still
requires the target-server checks above; passing local tests alone is not a
substitute for ARM64 Linux hardware validation.
