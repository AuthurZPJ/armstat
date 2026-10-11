<!-- SPDX-License-Identifier: GPL-2.0 -->
# armstat Reference

<p align="center">
  <a href="../README.md">README</a> | <a href="REFERENCE.zh-CN.md">简体中文</a>
</p>

This reference covers implementation, metric formulas, integration, and
validation. The [README](../README.md) covers normal use;
`man armstat` lists command-line options after installation.

## Repository layout

| Directory | Responsibility |
|---|---|
| `src/app/` | CLI and process lifecycle |
| `src/core/` | Collection, aggregation, CPU inventory, sampling deadlines |
| `src/platform/` | Linux/ARM telemetry backends |
| `src/output/` | Field registry, owned records, serializers |
| `scripts/` | Export loaders and plots |
| `tests/` | Regression tests and ARM64 target acceptance |
| `man/`, `docs/` | Installed manual and reference |

Headers are private interfaces beside their implementations; there is no
public C library API.

## Runtime architecture

```text
collect sys_snapshot -> calculate interval_stats -> build interval_record -> serialize
```

| Module | Owned work |
|---|---|
| `app/armstat_cli.c`, `app/armstat.c` | Options, startup, sampling loop, signals, output finalization, cleanup |
| `app/command.c` | Command process groups, launch gate, terminal access, signal forwarding, wait status |
| `core/collector.c` | Raw snapshot orchestration and shared accessors |
| `core/sample_cache.c` | Reusable buffers, cached reads, slow refresh |
| `core/aggregator.c` | Interval deltas, validity, CPU/package aggregation, baseline commit |
| `output/columns.c` | Field descriptors and visibility |
| `output/formatter_record.c`, `formatter_values.c` | Owned record values and typed accessors |
| `output/formatter_text.c`, `formatter_json.c`, `formatter_csv.c` | Serialization; machine helpers in `formatter_machine.c` |

Collection reads raw samples; aggregation derives interval deltas, percentages,
and rates. Serializers consume one self-contained record rather than retaining
pointers into mutable snapshot data.
Raw diagnostics use `output/formatter_raw.c` directly after one snapshot,
bypassing interval aggregation and the selected display fields.

The first snapshot establishes a baseline. Later samples use monotonic deadlines
anchored to it; collection/output time reduces the next wait. Fully missed
deadlines are skipped, and formulas always use measured `interval_delta_us`.

Command mode (`armstat [options] -- command [args...]`) completes initialization
before launch, then establishes a fresh baseline immediately before releasing
the child to exec. After wait/reap, one completion snapshot yields one interval.
The window includes small collection, launch, and wait overheads.
The child has its own process group; INT/TERM/HUP/QUIT are forwarded, and
foreground terminal access and shell stop/continue behavior are preserved.
Initialization failure prevents execution. CPU hotplug that resets the window,
collection failure, or output failure returns 1 without claiming a valid sample.
Successful measurement/output preserves the child status, including nonzero
exits: not found 127, other exec failures 126, signal termination 128+signal.
Statistics still describe selected CPU/platform activity, not per-process work.

### CPU identity

`cpu_id` is the real Linux ID used in paths and output. `tracked_idx` is the
dense index into the selected online set. `cpu_inventory.c` owns present,
online, and selected membership; use `for_each_tracked_cpu` for traversal.
`--cpu` filters collection and CPU-derived aggregates, while global counters
and platform sensors keep their own scope. IDs `0..1023` are representable;
larger inventories are reported as truncated.

Hotplug checks the compact online mask, avoiding directory scans until a change
or read failure; full scans use two observations to debounce changes. A rebuild
resets CPU-dependent caches, topology, PMU, and aggregation. The rebuild sample
is a new baseline; clocks are recaptured after it so rebuild time is excluded
from the next measurement.

### Sampling layers

| Layer | Data |
|---|---|
| Startup/rebuild | CPU membership, topology, sensors, idle-state names, PMU metadata |
| Slow refresh | Frequency limits, governor, boost, cpuidle disable flags |
| Per interval | Selected frequency, Busy/Idle, LPI, power, temperature, bandwidth, PMU inputs |

Slow data starts with a full refresh, then advances a rolling CPU cursor after
the fast snapshot. The default budget is `ceil(selected_cpus * interval / 5s)`,
clamped to 1–32 CPUs; this cap can lengthen a complete sweep beyond five seconds.
Runtime disable changes therefore take effect gradually.

Reads follow selected fields and scopes. `/proc/stat` is parsed once when
Busy/Idle/IOWait or system counters need it; LPI-only output does not need it.
cpuidle reads only selected states and counter families: residency does not
require usage, and usage alone does not require time. With no LPI demand,
collection skips cpuidle initialization and its availability warnings;
`--probe` discovers capabilities independently of `-s`/`-H`. Cached cpufreq/cpuidle
FDs and a cached procstat stream reduce repeated opens and parsing. Output
likewise skips CPU/package record construction when those scopes cannot emit.
A best-effort nice-value reduction before sampling is nonfatal if denied.

## Metric invariants and sources

Run `armstat --list` for exact field IDs, scopes, types, units, labels, and JSON
keys. Below, `Δ` means the difference between valid consecutive samples,
`T_us` is measured duration in microseconds, and `T_s = T_us / 1000000`.

| Fields | Source | Calculation / unit |
|---|---|---|
| `CPU`, `Pkg`, `Core`, `Node` | CPU inventory and Linux topology | Real IDs; CPU rows sorted by CPU ID |
| `Freq` | CPU `cpufreq/cpuinfo_cur_freq` | kHz / 1000 → MHz; current point sample |
| `Min`, `Max` | `scaling_min_freq`, `scaling_max_freq` | kHz / 1000 → MHz; configured limits |
| `Governor`, `Boost` | `scaling_governor`, per-CPU or global `cpufreq/boost` | Cached string / boolean |
| `UncoreFreq` | Unique matching `/sys/class/devfreq/<device>/cur_freq` | Hz / 1000000 → MHz; summary only |
| `Idle%` (procstat) | Per-CPU `/proc/stat` idle + iowait | Convert `Δ` from USER_HZ ticks to µs, then `/ T_us * 100` |
| `IOWait%` | Per-CPU `/proc/stat` iowait | Convert `Δ` to µs, then `/ T_us * 100` |
| `Busy%` | Selected utilization policy | `100 - Idle%`; schedstat uses runtime `Δ / elapsed_ns * 100` |
| `LPI-N`, `LPI-N_usage` | CPU `cpuidle/stateN/time`, `usage` | `Δtime_us / T_us * 100`; `Δusage / T_s` |
| `Power` | Unique `power_meter/power1_average` sensor | Mean of endpoint power samples in mW; summary scope |
| `Energy` | Derived from power | `Power_mW * T_s / 1000` → interval J |
| `Temp0` ... `Temp3`, CPU `Temp` | Temperature policy below | Milli-Celsius / 1000 → °C |
| `MemBW` | Unique `/sys/class/memory/mem*/mem_bytes_read` | `Δbytes / T_s / 1024²` → MiB/s |
| `CtxSw`, `IRQs`, `SoftIRQs` | `/proc/stat` ctxt, intr, softirq | `Δ` → count/interval |
| PMU, `IPC` | Per-CPU perf groups | Scaled interval counts; `instructions / cycles` |

Frequency at SUM/package scope averages valid current samples; it is not an
interval-average or busy frequency. Energy estimates a sampled signal using
endpoint averaging; it is neither a hardware energy counter nor a cumulative
value. System and PMU counts are not per-second rates. Display precision is
not a sensor-accuracy guarantee.

### Busy/Idle and LPI

Auto/procstat uses `/proc/stat` on all selected CPUs, including `nohz_full`.
Valid utilization is bounded to 0–100 and satisfies `Idle% + Busy% = 100`.
Procstat Idle includes iowait, so IOWait is already excluded from Busy.
Schedstat instead derives Idle from task runtime; IOWait remains a separate
procstat observation and is not guaranteed to be its subset in that mode.

Explicit schedstat is diagnostic: runtime settles at task switches and can
omit ongoing execution. The reader accepts versions 10–17, requires nine CPU
fields, and uses field 7 for runtime. Unsupported or invalid records fall back
to procstat per CPU. `task-clock` aliases this path. Short procstat intervals
show USER_HZ quantization; a jiffy delta that cannot safely convert to µs is
unavailable. The kernel accounting path is in
[`sched_info_depart()` / `sched_info_switch()`](https://github.com/torvalds/linux/blob/bd5f485f3f026225b86573e559af0b7254ef4184/kernel/sched/stats.h).

LPI states are independent sysfs statistics, with no residual redistribution,
100% cap, or default `IdleGap` field. Their sum need not equal Idle. Only states
0–7 are exposed; deeper states are not folded into another state. `--probe`
reports each `idle_state_N_name` mapping.

Time settles on state exit: a long idle period may report zero before exit and
more than 100% afterward. Software residency does not prove hardware depth.
Missing, disabled, failed, or reset state/counter values are independently
unavailable. Each SUM LPI state requires valid data from every selected CPU
before averaging; missing CPUs are not counted as zero.

### PMU

PMU collection is independent of raw counter display. Default, `-S`, `-a`,
`-s all`, and the `-s cpu` shortcut attempt IPC using `cycles,instructions`
without adding raw counter columns or CPU rows. This needs 2N perf descriptors
for N selected CPUs. If any CPU group fails startup, all automatic counters
are closed and IPC is hidden; only `-d` reports the failure. `-H ipc` suppresses
automatic collection. Other whitelists collect IPC only when selected.
`-P`/`--no-perf` prevents PMU initialization and skips the perf test in
`--probe`. Explicit PMU/IPC requests conflict with it regardless of option order;
Busy/Idle source rules remain unchanged.

Explicit `-I`, `-p`, or IPC field/group requests retain diagnostics and
unavailable columns. `-I` displays raw counts and IPC; an IPC selection alone
does not display raw counts. Custom `-p` lists suppress automatic IPC unless
it is explicitly requested, in which case cycles/instructions are added.
After automatic startup succeeds, subsequent invalid reads leave IPC visible
as unavailable, so PMU validity changes alone do not alter CSV columns.

Perf groups are opened per selected CPU. Each interval count is
`round(Δcount * Δtime_enabled / Δtime_running)` using exact integer arithmetic.
First, failed, short, reset, or unscheduled reads are unavailable. Invalid time
deltas, scaling overflow, or per-CPU cumulative-total overflow invalidate the
whole CPU group and start a new epoch; subsequent valid intervals can recover.

Summary counts sum per-CPU interval deltas, requiring every selected CPU to be
valid and rejecting 64-bit sum overflow. IPC requires valid named `cycles` and
`instructions` with nonzero cycles; summary IPC uses aggregate counts.

Event lists allow at most 16 unique names. Raw configs require `0x`/`0X` plus
hexadecimal digits within 64 bits, without signs or whitespace. `mem-read` and
`mem-write` are retired load/store counts, not bytes; optional cache events
depend on the CPU. See `--list` for the catalog. Explicitly requested events
denied by perf remain visible as unavailable. FD use grows with CPUs/events;
initialization attempts to raise soft `RLIMIT_NOFILE` within the hard limit.
Explicit requests warn if the budget remains insufficient; automatic IPC falls
back silently except in debug mode.

### Sensor mapping and missing data

Power and memory bandwidth require exactly one matching source. Zero or
multiple candidates leave the metric unavailable; `--probe` reports paths,
candidate counts, and ambiguity. Uncore devfreq discovery likewise requires a
unique matching source.

`ARMSTAT_TEMP_POLICY=thermal-zone-index` maps `thermal_zoneN/temp` to `TempN`
and NUMA/die N. CPU `Temp` uses its NUMA node's reading. Sparse IDs and negative
temperatures are preserved. `none` and `disabled` suppress summary temperature
discovery; other values fail startup. Verify this mapping on the server.
Per-core power/temperature and dedicated core aggregate rows are not provided.

Unavailable interval inputs stay unavailable throughout aggregation and
serialization. Failed reads invalidate dependent fields; cumulative sources
are re-baselined on recovery to avoid bridging a gap or publishing false zero.
A valid zero remains data.

## Output contract

JSON/CSV and bundled plot loaders use **schema version 9**. Version 8's LPI
fields were residual-adjusted; do not interpret them as independent state
statistics or mix those captures with version 9. `--list` is generated from
the serializer's field registry. The separate key-value probe contract is
`probe_schema_version: 1`; its PMU check covers cycles on the first CPU only.

### Column selection

Default output is SUM; `-a` adds CPU live metrics and Pkg/Core/Node topology,
without package aggregate rows, frequency configuration, or LPI usage.
`-s all` adds those omitted groups. These built-in views include available IPC;
raw PMU counts always require explicit requests.
`MemBW` requires explicit `-s membw` (or `mem`) or `-s mem_bw`; these can be
combined with other selections, such as `-s all,membw`. Default, `-S`, `-a`,
and `-s all` do not enable it.
`freq` selects current frequency/UncoreFreq, `freq-config` selects
Min/Max/Governor/Boost, `idle` selects residency and Busy/Idle/IOWait, and
`idle-usage` selects per-CPU state-entry rates.

Groups match without case and take precedence over exact stable IDs, which
take precedence over text/JSON aliases. Shared aliases select fields without
expanding row scopes. Stable CPU field IDs and the `cpu`, `core`, `numa`,
`freq-config`, and `idle-usage` groups enable CPU rows. Package field IDs and
`pkg`/`package` enable package rows. CPU-only aliases such as `Min`, `Governor`,
and `lpi0_usage` also enable CPU rows.
`-s cpu` alone supplies common CPU live metrics without SUM and preserves
explicit hide exclusions; combined selections use the requested fields and
scopes. Independent PMU/IPC/energy requests retain their required SUM output.
CPU filtering retains the selected-CPU SUM alongside expanded CPU rows;
the `-s cpu` shortcut stays CPU-only, and package rows follow selection.
Global counters and platform sensors keep their own scope.
The first `-s` replaces earlier views: `-a -s cpu` selects
the CPU shortcut; `-s cpu -a` selects the expanded live view.
Repeated `-s` extends the whitelist, including the full `-s all` selection,
without restoring unrelated groups hidden by `-H`, including explicitly
requested PMU, IPC, or energy fields.
If discovery leaves the selected view with only CPU row IDs and no data
columns, startup fails. Visible columns with unavailable sample values remain
valid selections.

### Stable interval metadata

| Field | Meaning |
|---|---|
| `schema_version` | Integer compatibility gate |
| `interval` | One-based visible sample number |
| `duration_us` | Measured monotonic window in microseconds |
| `timestamp` | Unix sample-end wall time in whole seconds |
| `timestamp_ns` | Same wall time in nanoseconds; preferred programmatic axis |
| `timestamp_iso` | Local RFC 3339 sample-end time with nine fractional digits |

Duration describes the measurement window; all three timestamps describe the
same end time. Do not use the requested interval as an exported rate denominator.

### Text

When SUM is the only output scope, text uses a continuous table with one
timestamped data row per interval, including default, `-S`, and matching `-s`
selections. Other views use interval markers, measured seconds, and blank
separators. Text timestamps retain the date/timezone and three fractional
digits. Headers include units
and repeat on label, selection, or width changes. Row-key widths accommodate
sparse package IDs. `-q`/`--quiet` hides only the startup banner;
`--quiet-all` also removes markers, timestamps, and headers. Machine keys
remain unchanged.

By default, text sent to a terminal with a reported height and width repeats
headers automatically. SUM-only output uses a screen-line budget across
intervals; other views start each section with a header every interval and repeat it
within long CPU/package tables. Terminal dimensions are read each interval
to follow resizing. Selected field, PMU, row-key, and SUM-only timestamp
widths estimate line wrapping; two screen lines are reserved. If one header
plus a data row cannot fit, automatic rolling repeats stop; startup, layout
changes, and section starts still print headers.
This controls header placement, without dropping columns, paging, or
redrawing the screen. Explicit `-N N` replaces this policy: positive N repeats
every N samples, while `-N 0` prints only at startup and on layout changes.
Pipes, redirection, and terminals without usable dimensions use that latter
behavior by default. `--quiet-all` suppresses interval headers in all cases.

`-D`/`--Dump` is a separate diagnostic text format. It collects the backend's
available proc/sysfs sources once without sleeping, ignoring view/field
selection; `-c` still limits CPUs. It includes cumulative procstat/schedstat
and cpuidle counters, CPU configuration, memory-read bytes, and current
sensors with labelled units. Dump reads schedstat without a source override
and includes up to 16 cpuidle states, beyond the regular eight LPI columns.
It calculates no interval delta, rate, percentage,
or energy, and opens no perf events. Its explanatory body remains visible with
quiet options; `-N` has no effect. Dump conflicts with explicit `-i`/`-n`,
a command, `--probe`, `-l`, non-text formats, and explicit PMU/IPC requests.
Use `-n 1` for a single measured interval.

### JSON

The top-level array contains one object per interval, with metadata and selected
`summary`, `packages`, and `cpus` sections. Packages use `package` identity;
CPUs use real Linux `cpu` IDs. Default mode and `-S` emit only summary; `-a`
adds CPU output, and `-s all` also adds packages. Explicit selection controls
the scopes: `-s pkg` (or `package`) and exact fields such as `pkg_freq_mhz`
request package rows.
CPU package topology remains available for identification and plotting.

Missing numbers/strings and non-finite values are `null`; available Boost is
boolean. Strings are JSON-escaped. The array is finalized on orderly exit;
abrupt termination can leave an incomplete document.

### CSV

One header defines the layout. Default output uses the summary-only layout:

| Selected scopes | Identity columns |
|---|---|
| Summary only | `Scope`, with value `SUM` |
| Package only | `Package` |
| CPU only | `CPU`, with real Linux IDs |
| Mixed | `Scope,CPU,Package`; rows ordered `SUM`, `PKG`, `CPU` |

Compact data columns use JSON keys; mixed columns add `summary.`, `package.`,
or `cpu.`. PMU columns are `pmu.<event>` or, in mixed output,
`summary.pmu.<event>` / `cpu.pmu.<event>`. Missing and out-of-scope cells are
empty; Boost uses 0/1. Commas, quotes, CR, and LF follow CSV quoting rules.
Use a conforming parser and select rows by identity.

Scopes and ordered columns must match the first header. A capability-induced
layout change returns an error before the affected interval; CPU membership
can change if the layout remains fixed. Restart CSV or use JSON when fields
can change.

### Output lifecycle

`-o`/`--out` chooses the statistics file. Periodic statistics use stdout by
default; command statistics use stderr while the child retains its original
stdout/stderr. Child stderr and statistics therefore share a stream by default;
use a file for command JSON/CSV. Command mode requires an explicit `--` and
rejects explicit `-i`/`-n`, `-D`, `--probe`, and `-l`.

Existing targets are opened only after initialization and baseline collection
succeed, with formatter allocation also required for interval output.
After startup, output streams progressively;
later failures retain the written prefix and return nonzero. All successful
operations, including help/list/probe, require a checked stdout flush. SIGPIPE
is handled as EPIPE, allowing resource cleanup before exit.

### CLI migration

Canonical aliases are `-n`/`--num_iterations`, `-N`/`--header_iterations`,
`-S`/`--Summary`, `-J`/`--Joules`, `-o`/`--out`, and `-D`/`--Dump`.
Long names are case-sensitive; previous spellings and `-O` are not accepted.
Probe is only `--probe`, and `-P` now disables perf. `-q` keeps the text table;
use `--quiet-all` for the former undecorated text output.

The CLI adopts selected turbostat conventions while retaining native views,
formats, and the one-second interval. `Freq` is a sysfs point sample, not
Avg_MHz/Bzy_MHz; default Busy is software procstat accounting, not hardware C0.
LPI columns remain independent of Idle%. Power is mW and `-J` adds estimated
interval joules alongside it. The README contains the compact migration table.

## Plotting exports

Plotting needs Python 3 and matplotlib, for example in an activated environment:

```bash
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install matplotlib
armstat -S -a -f json -o summary.json -i 1 -n 60
armstat -a -f csv -o cpus.csv -i 1 -n 60
armstat-plot-summary summary.json --y power --y2 temp0
armstat-plot-cpu cpus.csv --preset busy --top 8
```

Source-tree entry points are `python3 scripts/plot_sum.py` and
`python3 scripts/plot_cpu.py`. Both also accept JSON/CSV mixed exports that
contain their required scope. Use their `--help` and input `--list-fields`.

| Options | Use |
|---|---|
| `--preset`, or `--y` / `--y2` | Choose a metric preset or fields; do not combine them |
| `--sample-range START:END` | One-based sample window; CSV retains only that window, JSON loads fully |
| `--smooth N` | Rolling mean over N samples, preserving current missing samples |
| `--cpu-filter`, `--top`, `--group-by node/core` | Limit CPU lines or group them |
| `--aggregate avg/max/min`, `--rank-by avg/max/last` | CPU group aggregation and top-N ranking |
| `--output`, `--output-dir`, `--format png/svg/pdf` | Output location and image format |

Core grouping uses `(package, core)` identity. Average ranking is the arithmetic
mean of visible samples, not duration weighted. Entities with no primary data
are reported and skipped; a missing secondary field skips only that line.

Loaders reject unsupported/fractional schema versions, duplicate JSON keys or
CPU IDs, malformed sections, nonstandard numeric constants, noncontiguous
intervals, duplicate CSV columns, and row-width mismatches. Oversized numbers
outside floating-point range become gaps. Missing data and smoothing never
substitute stale samples.

Real time is used only when all selected timestamps are valid and increasing;
otherwise the whole axis uses sample numbers. Export timezone offsets are
preserved; changing offsets normalize to UTC. Parsing supports nine-digit
fractions and compact offsets such as `+0800` on Python 3.10 while retaining
original timestamp metadata.

Fields/axes show canonical units; long same-unit labels use `Value (unit)` with
field names in the legend. Legend space is measured after rendering, and the
summary palette covers all ten idle/LPI lines. Headless rendering writes a
temporary file beside the destination and replaces it atomically on success.
Existing permissions are preserved; new files use `0666 & ~umask`. Directory
or rendering failures report a clean error and preserve existing output.

## Build and validation

| Command | Purpose |
|---|---|
| `make`, `make clean` | Release build / clean selected output |
| `make test` | C/CLI/export/loader/streaming tests, optional render, build/install checks |
| `make debug`, `make debug-test` | ASan/UBSan build / complete sanitizer suite |
| `make analyze` | GCC path-sensitive analysis or Clang analyzer |
| `make target-test` | ARM64 Linux runtime and capability checks |

```bash
make O=/tmp/armstat-build
/tmp/armstat-build/armstat --version
make CROSS_COMPILE=aarch64-linux-gnu-
ARMSTAT_REQUIRE_PLOT_RENDER=1 make test
```

Build standalone or under Linux `tools/power/armstat`. `O` isolates binaries,
objects, dependency files, and tests. Configuration fingerprints include
compiler/target, flags, and `VERSION`, preventing stale reuse across build
changes; the selected binary is published atomically. Linux release builds
enable stack protection, PIE, RELRO, and immediate binding. Analyzer output is
separate and does not replace the release binary.

Tests suppress Python caches. `make O=... clean` preserves source caches;
in-tree clean removes them. A read-only source checkout can build/test with a
writable `O`. Verify actual artifacts as well as command exit status.

### ARM64 target acceptance

On each Kunpeng deployment model, inspect `--probe` and run under deployment
permissions. Busy/Idle coherence is mandatory; frequency is required for every
model claimed for deployment. Enable other capability requirements only when
the platform promises them:

```bash
./armstat --probe
ARMSTAT_REQUIRE_FREQ=1 make target-test
ARMSTAT_REQUIRE_FREQ=1 ARMSTAT_REQUIRE_PMU=1 \
ARMSTAT_SOAK_ITERATIONS=1000 ARMSTAT_SOAK_INTERVAL=1 make target-test
```

| Requirement flag | Checked capability |
|---|---|
| `ARMSTAT_REQUIRE_FREQ` | Positive summary frequency and at least one positive sample per selected CPU |
| `ARMSTAT_REQUIRE_PMU` | Cycles, instructions, and IPC |
| `ARMSTAT_REQUIRE_CPUIDLE` | Visible state-name mappings, summary residency, at least one valid CPU-scope `lpi0_usage` sample, plot ingestion |
| `ARMSTAT_REQUIRE_POWER`, `ARMSTAT_REQUIRE_TEMP` | Power / temperature |
| `ARMSTAT_REQUIRE_MEMBW`, `ARMSTAT_REQUIRE_UNCORE` | Memory bandwidth / uncore frequency |

The target test also checks options, output failures, probe structure, text,
JSON/CSV, timestamps, missing values, and resources. A soak checks Busy/Idle and
every required capability's valid-sample ratio, defaulting to at least 95%.
Tune `ARMSTAT_MIN_VALID_PERCENT`, `ARMSTAT_MAX_RSS_KIB`, `ARMSTAT_MAX_OPEN_FDS`,
and `ARMSTAT_MAX_DIAGNOSTIC_LINES` as needed. Without an explicit FD ceiling,
the default is 256. If PMU is required or the initial default capture contains
IPC, it becomes `max(256, tracked_cpus * 2 + 64)`, covering the event pair and
other runtime FDs. When PMU is not required and automatic IPC is unavailable,
the limit stays at 256; an explicit `ARMSTAT_MAX_OPEN_FDS` always takes precedence.

Release acceptance requires host tests, analysis, sanitizer tests, real plots,
capability-enforced target tests, inspected text/summary/JSON/CSV samples, and a
sustained run on the physical server. VM/container results prove portability
and contracts, not physical PMU, idle, power, temperature, or bandwidth behavior.

## Installation

```bash
sudo make install
make DESTDIR=/tmp/armstat-stage PREFIX=/usr install
make DESTDIR=/tmp/armstat-stage PREFIX=/usr uninstall
```

Default `PREFIX` is `/usr`. Installation places the collector and two plot
commands in `bin`, shared plot modules in `share/armstat`, the manual in
`share/man/man8`, and README/reference pairs, `VERSION`, and unmodified
`COPYING` under `share/doc/armstat`.

## Documentation maintenance

Keep behavior updates synchronized in this order:

```text
code -> README.md -> README.zh-CN.md -> man/armstat.8
     -> docs/REFERENCE.md -> docs/REFERENCE.zh-CN.md -> tests
```

Keep usage in README, full formulas/contracts here, options in the manual, and
contribution rules in AGENTS.md. Update existing documents before adding a
file; retain meaningful limits and failure behavior when shortening prose.
