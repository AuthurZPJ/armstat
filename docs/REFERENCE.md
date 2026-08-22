<!-- SPDX-License-Identifier: GPL-2.0 -->
# armstat Reference

<p align="center">
  <a href="../README.md">README</a> |
  <a href="REFERENCE.zh-CN.md">简体中文</a> |
  <code>armstat(8)</code>
</p>

This is the single maintainer and integration reference for `armstat`. The
README explains normal use; this document collects architecture, export,
plotting, and release-validation details that should not be spread across
separate files.

## Repository layout

```text
src/app/       command-line parsing and process lifecycle
src/core/      collection orchestration, interval aggregation, CPU inventory
src/platform/  Linux and ARM telemetry backends
src/output/    field registry, record materialization, text/JSON/CSV output
scripts/       export loaders and plotting tools
tests/         host-independent regression and ARM64 target acceptance tests
man/           installed manual page
docs/          this reference
```

Headers are private project interfaces and stay beside their implementation
area. The project does not currently publish a C library API.

## Runtime architecture

The sampling path has three stages:

```text
collect raw snapshot -> aggregate interval deltas -> materialize and serialize
```

- `src/app/armstat_cli.c` parses options and column selection before runtime
  initialization.
- `src/app/armstat.c` owns startup, the monotonic sampling loop, signals,
  output-file finalization, and shutdown.
- `src/core/collector.c` coordinates one raw snapshot. It does not format data
  or calculate user-facing interval percentages.
- `src/core/aggregator.c` converts cumulative inputs into one
  `interval_stats`. Its public entry point is deliberately a short ordered
  pipeline: initialize, frequency, Busy/Idle, system metrics, PMU, package
  aggregation, then baseline commit.
- `src/output/formatter_record.c` copies the snapshot and interval statistics
  into an owned `interval_record`.
- `src/output/formatter_values.c` exposes the typed, read-only accessors used
  by the field registry, separate from allocation and materialization.
- `src/output/formatter_text.c`, `formatter_json.c`, and `formatter_csv.c`
  serialize that record. `formatter_machine.c` contains only helpers shared by
  the JSON and CSV streams.

The first collection establishes cumulative baselines. A visible sample is
emitted only after one complete interval. Sampling deadlines are anchored to
the monotonic clock; normal work time is deducted from the next wait and a
fully missed deadline is skipped instead of producing catch-up bursts. Metric
formulas use the measured `interval_delta_us`, not the requested interval.

### CPU identity

Two CPU identities are intentional:

- `cpu_id` is the real Linux CPU number used at kernel interfaces and in
  output.
- `tracked_idx` is a dense internal index into the selected online CPU set.

`src/core/cpu_inventory.c` owns present, online, and tracked membership. The
`for_each_tracked_cpu` iterator is the normal internal traversal interface.
`--cpu` is a sampling filter, so unselected CPUs do not consume per-CPU PMU or
sysfs resources. The compiled representation accepts CPU IDs `0..1023`; a
larger or higher-numbered online set is reported as truncated.

When online membership changes, the runtime rebuilds CPU-dependent caches and
topology, resets the aggregator, and consumes a new baseline before producing
another row. The rebuild interval is not presented as a normal measurement.

### Sampling layers

- Static/rebuild data: CPU membership, topology, sensor paths, cpuidle state
  identity, and PMU metadata.
- Slow-changing data: frequency limits, governor, boost, and cpuidle enable
  state, refreshed with a rolling budget.
- Per-interval data: current frequency and Busy/Idle inputs, plus column-gated
  cpuidle split counters, package power, temperature, memory bandwidth, and
  PMU counters.

`src/core/sample_cache.c` owns reusable storage and cached descriptors.
`src/core/collector.c` exposes snapshot accessors for fields consumed by more
than one downstream module; the snapshot is not yet fully opaque.

## Metric invariants and sources

The README contains the complete user-facing field list. These rules are the
maintainer invariants that calculations and serializers must preserve:

- `Idle% + Busy% = 100%` for every valid CPU interval.
- `IOWait%` is an independent `/proc/stat` view and is not subtracted from
  `Busy%`.
- With `--busy-source=auto`, ordinary CPUs use `/proc/stat`; CPUs identified
  as `nohz_full` use `/proc/schedstat` runtime when that input is valid.
- Split `LPI-*` columns are a display decomposition of authoritative idle
  time. The deepest visible usable state absorbs the residual so displayed
  states approximate `Idle%` rather than claiming raw cpuidle counters are a
  second Busy/Idle authority.
- `SUM` percentages and frequency are averages across valid tracked CPUs;
  system counters and PMU counters are interval deltas or aggregates.
- Package rows group tracked CPUs by the physical package ID supplied by
  topology.
- `Freq` is the current `cpuinfo_cur_freq` sample. Summary and package values
  are cross-CPU averages of the current samples, not time averages or
  hardware-counter-derived effective frequencies.
- Energy is derived from interval-average package power and measured duration.
- IPC is emitted only when named `cycles` and `instructions` deltas are both
  valid and cycles is nonzero.

Primary kernel interfaces are:

| Metric | Source | Output unit |
|---|---|---|
| current/min/max frequency, governor, boost | CPU `cpufreq` sysfs | MHz/string/bool |
| Busy/Idle and IOWait | `/proc/stat`, optionally `/proc/schedstat` | % |
| split idle states and usage rates | CPU `cpuidle` sysfs | %, /s |
| package power | one unambiguous `power_meter`/`power1_average` source | mW |
| interval energy | derived from power and duration | J |
| temperature | selected `thermal_zone` policy | degC |
| memory bandwidth | one unambiguous platform counter | MiB/s |
| context switches and interrupts | `/proc/stat` | count/interval |
| PMU and IPC | `perf_event_open()` | count/interval, instructions/cycle |

Power and memory-bandwidth discovery require exactly one matching source.
Ambiguous discovery remains unavailable and is explained by `--probe`; the
implementation does not select a source based on directory enumeration order.
PMU normally requires root or a permissive `perf_event_paranoid` setting.

Unavailable telemetry must remain unavailable across the pipeline. A valid
zero is data; it must not be used as a substitute for a failed read, a
re-baselining interval, or an unsupported capability.

## Output contract

Machine-readable output currently uses `schema_version = 8`. Run
`armstat --list` for the authoritative current field IDs, scopes, types, units,
text labels, and JSON keys; that view is generated from the same field
registry used by the serializers.

### Stable interval metadata

Every JSON interval and every CSV row starts with equivalent metadata:

| Field | Meaning |
|---|---|
| `schema_version` | integer compatibility gate |
| `interval` | one-based visible interval number |
| `duration_us` | measured monotonic sampling window in microseconds |
| `timestamp` | Unix wall time in whole seconds |
| `timestamp_ns` | Unix wall time in nanoseconds; preferred programmatic axis |
| `timestamp_iso` | RFC 3339 local timestamp with nine fractional digits |

`timestamp`, `timestamp_ns`, and `timestamp_iso` describe the same sample end
time. `duration_us` describes the measurement window, not wall-clock display
precision.

### JSON

JSON is one top-level array with one object per visible interval. Each object
always contains the metadata above and may contain:

- `summary`: enabled system-scope fields and optional aggregate `pmu` object;
- `packages`: one object per package, each identified once by `package`;
- `cpus`: one object per tracked CPU, each identified by real Linux `cpu`.

Section presence follows the selected level. The default emits `summary` and
`packages`; `-S` emits only `summary`; `-a` expands the output with `cpus`.
Explicit selection can emit any valid combination.

Unavailable numbers and strings are JSON `null`. Available `boost` values are
JSON booleans. Non-finite internal floating-point values are normalized to
`null`, so the output never depends on non-standard `NaN` or infinity tokens.
Strings are JSON-escaped.

### CSV

The header is emitted once. After stable metadata, CSV uses one of four
layouts:

- summary-only: identity column `Scope`, value `SUM`;
- package-only: identity column `Package`;
- CPU-only: identity column `CPU` containing the real Linux ID;
- mixed-scope: identity columns `Scope,CPU,Package`, with rows ordered `SUM`,
  `PKG`, then `CPU` for each interval.

Mixed headers qualify data columns as `summary.<field>`, `package.<field>`, or
`cpu.<field>`. PMU fields use `summary.pmu.<event>` and `cpu.pmu.<event>`.
Cells outside a row's scope and unavailable values are empty; valid zeroes
remain zero. Fields containing a comma, quote, newline, or carriage return are
quoted according to CSV rules, including doubling embedded quotes.

Do not split CSV on commas manually. Use a conforming CSV parser and select
rows by their identity fields.

### Units and compatibility

Canonical units are MHz, percent, degC, mW, J, MiB/s, usage delta per second,
count per interval, and instructions per cycle. Rendered decimal precision is
a presentation choice, not a sensor-accuracy guarantee.

Consumers must require `schema_version = 8`; the project has not shipped an
older contract that needs compatibility. Consumers must preserve missing
values rather than converting them to zero.

## Plotting exports

The optional scripts require Python 3 and matplotlib:

```bash
python3 -m pip install matplotlib
```

Create summary or CPU inputs with:

```bash
armstat -S -f json -O summary.json
armstat -S -f csv -O summary.csv
armstat -a -f json -O cpus.json
armstat -a -f csv -O cpus.csv
```

`scripts/plot_sum.py` handles summary series; `scripts/plot_cpu.py` handles
per-CPU and grouped series. Both use `scripts/armstat_loader.py` for schema
validation, field aliases, missing-value handling, and timestamps.

```bash
python3 scripts/plot_sum.py summary.json --preset freq
python3 scripts/plot_sum.py summary.json --y power --y2 temp0
python3 scripts/plot_cpu.py cpus.json --preset busy --top 8
python3 scripts/plot_cpu.py cpus.csv --group-by node --y busy
```

Use `--list-fields` to inspect available series. CPU plots should normally use
`--cpu-filter`, `--top`, or `--group-by` on large machines. JSON inputs are
loaded in full; CSV with `--sample-range` is streamed so only the requested
sample window is retained. Missing values become `NaN` and appear as gaps.

## Build and validation

Normal development gates are:

```bash
make clean
make
make test
make debug-test
make analyze
```

`make test` covers C calculation and policy tests, CLI/error-path tests,
text/JSON/CSV contracts, streaming and plotting loaders, and build/install
transitions. `make debug-test` rebuilds and runs the suite with AddressSanitizer
and UndefinedBehaviorSanitizer. On Linux with GCC, `make analyze` runs the
path-sensitive static analyzer in `.armstat-analysis/`.

Out-of-tree builds use `O` and must keep generated files under that directory:

```bash
make O=/tmp/armstat-build
/tmp/armstat-build/armstat --version
```

### ARM64 target acceptance

Host-independent tests cannot prove hardware behavior. On each supported
Kunpeng ARM64 Linux deployment model, first inspect capabilities and then run:

```bash
./armstat --probe
make target-test
```

The target test checks basic options, output failures, probe structure,
default/summary/JSON/CSV execution, timestamps, missing values, process
resources, and optional telemetry when explicitly required. Capability gates
are enabled with:

```bash
ARMSTAT_REQUIRE_PMU=1 \
ARMSTAT_REQUIRE_CPUIDLE=1 \
ARMSTAT_REQUIRE_POWER=1 \
ARMSTAT_REQUIRE_TEMP=1 \
ARMSTAT_REQUIRE_MEMBW=1 \
ARMSTAT_REQUIRE_UNCORE=1 \
make target-test
```

Set only the requirements promised by that deployment platform. A stability
run can be requested with `ARMSTAT_SOAK_ITERATIONS` and
`ARMSTAT_SOAK_INTERVAL`; resource ceilings are configurable through
`ARMSTAT_MAX_RSS_KIB`, `ARMSTAT_MAX_OPEN_FDS`, and
`ARMSTAT_MAX_DIAGNOSTIC_LINES`.

Before release, require all host gates, the capability-appropriate target
test, valid default text plus `-S`, JSON and CSV samples, and a sustained run
on the actual server. Container or virtual ARM64 execution proves portability
and contracts, but not real PMU, cpuidle, power, temperature, or bandwidth
semantics.

## Installation

`make install` installs:

- `armstat` under `PREFIX/bin`;
- `man/armstat.8` under `PREFIX/share/man/man8`;
- the README pair under `PREFIX/share/doc/armstat` and this reference pair
  under `PREFIX/share/doc/armstat/docs`;
- plotting and loader scripts under `PREFIX/share/doc/armstat/scripts`.

The default `PREFIX` is `/usr`; staging with `DESTDIR` is supported.

## Documentation maintenance

Keep user-visible behavior synchronized in this order:

```text
code -> README.md -> README.zh-CN.md -> man/armstat.8
     -> docs/REFERENCE.md -> docs/REFERENCE.zh-CN.md -> tests
```

Prefer updating an existing section over creating another Markdown file. A new
document is justified only when it has a distinct audience, lifecycle, or
installation destination that cannot be served by the README, manual page, or
this reference.
