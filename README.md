# armstat
<p align="center">
  <b>English</b> | <a href="README.zh-CN.md">简体中文</a>
</p>

`armstat` is an ARM server monitoring tool in the style of `turbostat`.
It focuses on interval-based observation of:

- CPU frequency
- Busy/idle time
- package power and energy
- NUMA/package temperature
- PMU counters and IPC
- topology metadata (package/core/NUMA)

It is designed for ARM64 servers where the available telemetry comes from a
mix of `sysfs`, `/proc/stat`, `hwmon`, `thermal_zone`, and `perf_event_open()`.

**Key differentiator**: armstat produces machine-readable JSON and CSV exports
with per-sample timestamps and a stable `schema_version`. Helper plotting
scripts are included, so you can go from `armstat -f json -O data.json` to
time-series charts without manual data wrangling. See the
[integration reference](docs/REFERENCE.md#output-contract).

Current exports use `schema_version = 7`. Version 5 introduced explicit
unavailable telemetry, and version 6 added unambiguous package CSV rows.
Version 7 adds the measured `duration_us`, emits RFC 3339 timestamps, makes
Boost a JSON boolean, applies missing-value semantics to strings, and removes
redundant package identity fields.

## Documentation

- **[REFERENCE.md](docs/REFERENCE.md)** - Architecture, export contract,
  plotting, and release validation
- **`armstat(8)`** - Installed command-line manual (`man armstat`)

## Quick Start

The collector targets ARM64 Linux and expects mounted `/proc` and `/sys`.
Building requires a C compiler and `make`; plotting is optional and requires
Python 3 plus matplotlib.

```bash
make
./armstat --probe
./armstat -i 1 -n 5
./armstat -S -a -i 1 -n 5
./armstat -S -a -f json -O armstat.json -i 1 -n 5
sudo make install
```

Review `--probe` before treating missing optional fields as a defect. PMU/IPC
normally requires root or a permissive `perf_event_paranoid` setting. Before a
production rollout, run the capability-enforced target procedure in
[REFERENCE.md](docs/REFERENCE.md#arm64-target-acceptance).

## Current Output Model

`armstat` uses a `SUM + package + CPU rows` model:

- Default text mode prints one row per tracked CPU (no summary or package rows)
- `-a` enables all supported base column groups and adds the package aggregation rows plus the `SUM` summary row on top of the per-CPU rows
- `-S` prints a single `SUM` row per interval
- JSON writes an array of interval objects
- CSV writes summary-only, package-only, CPU-only, or explicitly scoped mixed
  rows, depending on the selected fields

This is intentionally `turbostat`-like, but it is not a byte-for-byte clone of
the x86 tool. In particular, ARM platforms often do not expose a uniform
hardware power/thermal/idle model comparable to x86 MSR/RAPL/TSC.

## Mental Model

The easiest way to understand one visible `armstat` line is:

1. establish a baseline sample
2. wait one full interval
3. collect a new raw snapshot
4. derive interval deltas and percentages
5. format those derived values as `SUM`, per-package rows, per-CPU rows, or a
   selected combination

That means the first visible line is always "one complete interval later", not
"instantaneous state at program start".

Subsequent samples follow monotonic-clock deadlines anchored to the baseline.
Normal collection/formatting overhead is deducted from the next wait instead
of accumulating as cadence drift; if work overruns an entire interval, armstat
skips the missed deadline rather than issuing burst catch-up samples. Metric
formulas always use the measured interval delta.

If armstat has to rebuild runtime state (for example after CPU topology change),
the rebuild sample becomes a new baseline and is not printed as a normal
interval row.

Three ideas help interpret the output correctly:

- `Busy%` / `Idle%` answer "how much of this interval looked busy or idle?"
- `LPI-*` answers "when the CPU looked idle, how was that idle residency split?"
- `SUM` answers "what is the average or aggregate view across tracked CPUs?",
  not "one special pseudo-CPU"

## Key Semantics

### CPU numbering

- External CPU identity is always the real Linux CPU ID
- Internal arrays use a dense tracked index
- Output rows are sorted by real CPU ID
- `--cpu` filtering accepts real CPU IDs and ranges such as `0,1,4-7`
- `--cpu` is a sampling filter, not only an output filter: only online CPUs
  matching the list become tracked CPUs, and cpufreq, cpuidle, PMU, per-CPU
  Busy/Idle inputs, and tracked-CPU averages are based on that filtered set
- invalid CPU-list tokens, reversed ranges, and filters that match no online
  CPU fail at startup
- this build represents Linux CPU IDs `0..1023`; if sysfs reports more online
  CPUs or higher IDs, armstat preserves the actual online count for diagnostics,
  warns that sampling was truncated, and samples only representable IDs

### Idle and busy

- `Idle%` / `Busy%` are driven by the selected busy-source policy
- raw authoritative Busy/Idle inputs are captured once per interval in
  `sample_cache.c`, then converted to percentages in `aggregator.c`
- the default policy is `auto`:
  - ordinary CPUs use `/proc/stat`
  - CPUs listed in `/sys/devices/system/cpu/nohz_full` prefer `/proc/schedstat`
    runtime accounting when available
- `--busy-source procstat` forces `/proc/stat`
- `--busy-source schedstat` uses `/proc/schedstat` runtime accounting where
  available, with per-CPU fallback to `/proc/stat`
- `--busy-source task-clock` is accepted as a legacy compatibility alias and
  currently resolves to the same implementation as `schedstat`
- `IOWait%` is also derived from `/proc/stat` and represents the interval share
  spent in iowait accounting; Linux reports iowait within the idle counter, so
  it is included in `Idle%` and is not counted as busy
- Per-state idle residency and wakeup columns use cpuidle `stateN/name`
  labels such as `LPI-0`, `LPI-1`, ..., and `LPI-0_wake` (wakeups/s)
- cpuidle is used for split `LPI-*` residency only
- Per-state idle columns are hidden when cpuidle data is unavailable
- a transient cpuidle counter failure or reset makes the visible LPI set
  unavailable until a fresh baseline has been established
- a missing or disabled state reports its wakeup rate as unavailable, not zero
- The formatter exposes up to eight `LPI-*` columns (`LPI-0` ... `LPI-7`);
  platforms with deeper cpuidle state inventories are folded into the deepest
  visible usable residual bucket
- `Busy%` is calculated as `100 - Idle%`
- The visible `LPI-*` columns are display-oriented, not a pure dump of raw
  cpuidle percentages:
  - shallower usable states keep their raw cpuidle residency percentages
  - the deepest visible usable state becomes a residual bucket so that the
    visible `LPI-*` columns sum to `Idle%` for that CPU
  - if a deeper state is unavailable, disabled, or beyond the visible column
    limit, the residual is assigned to the deepest remaining visible usable
    state instead
- As a result, the deepest visible `LPI-*` value may be adjusted and should be
  interpreted as "remaining idle residency after shallower states", not always
  as the raw `stateN/time` percentage for that state

### What `SUM` means

`SUM` is intentionally not a dump of one special CPU. It is built from summary-
scope fields:

- frequency, power, energy, membw, PMU, and system counters are aggregated at
  summary scope
- `Idle%`, `Busy%`, and `IOWait%` are summary percentages derived from the
  aggregated interval stats
- summary `LPI-*` columns are averages of the displayed per-CPU `LPI-*` values,
  not a second independent global residual calculation

So when reading `SUM`:

- PMU/system counters behave like whole-machine interval totals or aggregates
- percentage-style fields behave like tracked-CPU averages
- with `--cpu`, tracked-CPU-derived `SUM` fields such as frequency, idle, LPI,
  and PMU are based on the filtered tracked CPU set; platform/global fields
  keep their natural summary scope
- the automatic mixed `SUM` section is still suppressed when `--cpu` is used,
  so filtered CPU rows are not silently shown beside a summary section
- aggregate sections are suppressed when they would be implicitly mixed beside
  filtered CPU rows; an explicit aggregate-only request such as
  `-s pkg_avg_freq --cpu 0-3` remains visible and uses the filtered tracked set

### Power and temperature

The current implementation is optimized for platforms with:

- package power from one uniquely identifiable `hwmon` `name=power_meter`,
  using `power1_average`
- summary temperatures from `thermal_zoneN/temp`, where `N` maps to NUMA/Vdie `N`
  under the current `thermal-zone-index` summary-temp policy

That means:

- `Power` is a `SUM`-scope field and is reported in mW
- `Temp` in CPU rows is derived from the CPU's NUMA node temperature and is reported in C
- `Temp0` ... `Temp3` are summary fields shown for discovered NUMA/Vdie zones
  and are reported in C
- sparse NUMA node IDs are preserved (for example node 2 maps to `Temp2`), and
  signed milli-Celsius readings are accepted
- per-core power and per-core temperature are currently not exposed

The summary-temperature policy is explicit rather than implicit:

- default policy: `thermal-zone-index`
- behavior: `thermal_zoneN/temp -> TempN -> NUMA/Vdie N`
- override: `ARMSTAT_TEMP_POLICY=none` disables summary `TempN` discovery
- accepted policy values are `thermal-zone-index`, `none`, and `disabled`;
  unknown values are startup errors rather than silent temperature loss

`--probe` reports the effective `summary_temp_policy` so platform assumptions
are visible at runtime. It also reports the number of package-power and
memory-bandwidth source candidates; more than one candidate is treated as
ambiguous and disabled instead of selecting an arbitrary directory entry.

### PMU

PMU support uses `perf_event_open()`:

- counters are opened per tracked CPU
- events are grouped per CPU
- group reads include `time_enabled` and `time_running`
- multiplexed counters are scaled before interval deltas are derived
- `-I` enables `cycles,instructions` and adds IPC columns
- `-p ...` enables PMU counters without implicitly enabling IPC
- IPC is available only when the active event list contains both named events
  and the interval has a nonzero cycle count
- unknown or duplicate PMU event names and event lists longer than
  `MAX_PMU_EVENTS` fail immediately; duplicate names are forbidden because
  machine-readable PMU objects use event names as keys, while perf
  permission/open failures still degrade to visible unavailable values for
  requested columns
- the first group read, a failed/short group read, zero running time, or a
  counter reset is an unavailable interval; recovery re-baselines before
  publishing another delta, so a gap is never compressed into one interval
- PMU file descriptor use scales with both event count and filtered tracked CPU
  count. Before opening groups, armstat best-effort raises its soft
  `RLIMIT_NOFILE` within the existing hard limit; if the budget is still short,
  it warns and reducing either CPUs or events lowers the pressure

PMU monitoring usually requires root or a permissive
`/proc/sys/kernel/perf_event_paranoid` setting.

If PMU or IPC columns are explicitly requested (for example via `-p`, `-I`,
`-s pmu`, or `-s ipc`) but no `-p` event list is
provided, armstat defaults to `cycles,instructions`.

The ARMv8 raw aliases use architectural PMUv3 event numbers. In particular,
`mem-read` and `mem-write` count retired loads and stores; they are event counts,
not transferred bytes or memory bandwidth. Whether optional cache-level events
are implemented is CPU-specific, so an architecturally named event can still be
unavailable on a particular machine.

### nohz_full and Busy/Idle

`nohz_full` CPUs can make short-interval `/proc/stat` busy/idle percentages
look erratic. For that reason, the default `auto` busy-source policy prefers
`/proc/schedstat` runtime accounting on CPUs listed in
`/sys/devices/system/cpu/nohz_full`, while continuing to use `/proc/stat` on
ordinary CPUs.

The schedstat reader follows the documented nine-field CPU record and uses
field 7 (task runtime in nanoseconds). Known schedstat versions 10 through 17
are accepted; an unknown version falls back to `/proc/stat` rather than
guessing at a field layout.

## Architecture

The source tree follows four responsibility boundaries:

```text
src/app/       CLI parsing and process lifecycle
src/core/      collection orchestration, aggregation, and CPU inventory
src/platform/  Linux and ARM telemetry backends
src/output/    field registry, records, and text/JSON/CSV serializers
```

The detailed ownership and data flow are maintained in
[REFERENCE.md](docs/REFERENCE.md#runtime-architecture), avoiding a second
per-file architecture list in the README.

## Optimization Strategy

The current implementation is optimized around "cheap enough for large ARM
systems" rather than "read everything on every interval".

### 1. Three sampling layers

- **Static / rebuild layer**:
  CPU inventory, topology, sensor discovery, cpuidle state names, PMU event
  metadata
- **Slow-changing layer**:
  CPU min/max frequency, governor, boost, and cpuidle `disable` state
- **Per-interval fast path**:
  current frequency, `/proc/stat` deltas, package power, NUMA temperatures,
  PMU counters, cpuidle `stateN/time`

This keeps the expensive "what exists on this platform?" work out of the hot
path.

Hotplug detection reads the compact Linux `online` CPU mask on each interval.
When its represented count, full count, and actual membership match the cached
catalog, armstat skips directory enumeration; a full inventory scan and the
two-observation debounce run only after a real change or an unreadable mask.

### 1.5. Busy/Idle execution path

The current Busy/Idle path is intentionally explicit:

1. `sample_cache.c` captures raw cumulative counters once per interval
2. the chosen busy-source policy decides which raw counter family is
   authoritative for each CPU:
   - `/proc/stat idle/iowait`
   - or `/proc/schedstat` runtime on selected CPUs
3. `aggregator.c` converts those cumulative counters into interval deltas
4. `Idle%`, `Busy%`, and `IOWait%` are derived from those deltas

This matters because `armstat` no longer keeps a hidden runtime idle backend
object with its own private "previous sample" state. Busy/Idle now lives on
the same explicit delta timeline as PMU, power, and system counters.

### 2. Budgeted slow refresh

Slow-changing data is not refreshed all at once. `sample_cache.c` advances a
cursor and refreshes only a budgeted subset of CPUs per interval. This avoids
periodic CPU usage spikes on large systems.

Concretely, the slow layer works like this:

- on first use or after a hotplug rebuild, it performs one full refresh so the
  cache starts from a complete baseline
- after that, each interval computes a CPU refresh budget instead of rescanning
  every tracked CPU
- the budget is derived from the sampling interval and the target sweep window
  (`SLOW_TARGET_SWEEP_MS`, currently about 5 seconds):
  `budget ~= tracked_cpus * interval_us / target_sweep_us`
- that budget is clamped to a sane range (`SLOW_MIN_CPU_BUDGET ..
  SLOW_MAX_CPU_BUDGET`) so very short intervals still make progress and very
  large machines do not refresh too much at once
- a rolling `slow_cursor` remembers where the previous interval stopped
- each interval refreshes CPUs `[slow_cursor, slow_cursor + budget)` and then
  advances the cursor, wrapping at the tracked CPU count
- cpuidle `disable` cache refresh uses the same budgeted model, so changing a
  state's `disable` file at runtime is reflected gradually instead of causing a
  full-machine metadata rescan
- the slow layer runs **after** the fast-path snapshot, so interval-critical
  metrics are collected first and low-frequency housekeeping cannot elongate the
  critical part of the sample

This means slow data is eventually refreshed across the whole machine, but the
cost is spread over many intervals instead of appearing as a periodic spike.

### 3. Cached kernel interfaces

- `cpufreq` current-frequency reads use cached file descriptors
- `cpuidle stateN/time` uses lazy file-descriptor caching
- `/proc/stat` uses a cached `FILE *` and a single parsed snapshot per interval

This reduces repeated `open()/close()` and repeated parsing overhead.

### 3.5. Best-effort process priority boost

Before entering normal interval sampling, `armstat` makes a best-effort
attempt to raise its own scheduling priority by setting a lower nice value
(matching the intent used by `turbostat`). This is not required for correct
functionality: if the kernel refuses the change, sampling continues normally.

### 3.6. Busy-source policy and nohz_full

The default busy-source policy is `auto`:

- use `/proc/stat` on ordinary CPUs
- use `/proc/schedstat` runtime accounting on `nohz_full` CPUs when available
- fall back per CPU to `/proc/stat` if schedstat is unavailable

The `task-clock` option remains as a user-visible compatibility alias, but it
now uses the same implementation as `schedstat` because CPU-wide perf
`task-clock` was not a reliable Busy/Idle source on the target ARM servers.

`idle_backend.c` now only owns this policy choice. It does not collect
interval percentages itself.

### 4. Demand-driven sampling

Not every source is refreshed on every interval:

- cpuidle `LPI-*` data is only refreshed when idle-state columns are visible
- power/energy sampling is only done when a visible field needs package power
- temperature sampling is only done when temperature fields are visible
- PMU sampling only runs when PMU is active

### 5. PMU grouping and scaling

PMU events are opened as perf groups per tracked CPU. Group reads include
`time_enabled` / `time_running`, and multiplexed intervals are scaled before
interval deltas are derived. Per-CPU validity is kept with each group read, and
the summary PMU object is available only when every tracked CPU contributed a
complete interval.

### 6. Three-part output pipeline

Formatting is split into:

1. `columns.c`: column visibility (`show_*` flags) + field descriptor table
2. `formatter_record.c`: build a stable `interval_record`;
   `formatter_values.c` provides its typed field accessors
3. `formatter_text.c`, `formatter_json.c`, and `formatter_csv.c`: serialize
   that record; `formatter_machine.c` holds their shared machine-output helpers

This keeps formatting logic consistent across text, JSON, and CSV without
recomputing metrics in serializers.

## Build

```bash
make
make test
make debug-test
make analyze        # GCC static analyzer on Linux
make target-test    # ARM64 Linux host runtime acceptance
make O=/path/to/output
```

armstat can be built standalone from this repository, or placed inside the
Linux source tree at `tools/power/armstat` and built there with the same
`make` invocation. Cross-compilation is supported via `CROSS_COMPILE`
(for example `CROSS_COMPILE=aarch64-linux-gnu-`), and out-of-tree builds
via `make O=/path/to/output`. Out-of-tree builds keep the binary, objects,
dependency files, and test binaries under `O`. Build configuration changes
(such as switching between release and sanitizer flags, compiler versions, or
compiler target architectures) invalidate incompatible objects automatically.
Objects and linked binaries are isolated by a build configuration fingerprint,
so parallel target execution and rapid release/debug/release switches do not
reuse stale objects; the selected binary is published to `armstat` atomically.
Linux release builds also enable stack-protector, PIE, RELRO, and immediate
symbol binding by default. The user-facing version comes from the repository's
single `VERSION` file and is included in the build fingerprint and installed
documentation. `make install` also ships the complete GPL-2.0 license text.
`make analyze` keeps its analyzer-only binary under `.armstat-analysis/`; it
does not clean or replace the release `armstat` selected by the normal build.

## Usage

### Basic

```bash
armstat
armstat -i 5
armstat -n 10
armstat -D
armstat --busy-source auto
armstat --busy-source procstat
armstat --busy-source schedstat
armstat --busy-source task-clock
```

`-n 0` (the default) runs until interrupted. `-D` still waits for and measures
one complete interval; it is not an instantaneous point-in-time snapshot.
Text `-D` output keeps its banner and column header so the one-shot result is
self-describing. Add `-q` explicitly when headerless text is required.

The minimum supported interval is one microsecond (`0.000001` seconds); smaller
values are rejected because collector timestamps and exported intervals use
microsecond resolution. The text startup banner preserves that subsecond
precision instead of rounding a valid short interval to zero.

### Other options

- `-N, --header-iterations N` — reprint text header every N intervals
- `-J, --joules` — show interval energy in Joules
- `-q, --quiet` — suppress interval banner and text headers
- `-h, --help` — show the complete command-line summary and exit
- `-v, --version` — show version and exit

### Output format

```bash
armstat -f text
armstat -f json
armstat -f csv
armstat -f json -O armstat.json
armstat -f csv -O armstat.csv
```

`-O` / `--export` is a file-export alias for `-o` / `--output`. It is
especially convenient for machine-readable output such as JSON and CSV, but it
works with text output as well.

An existing output file is not truncated until collector initialization and
the baseline sample have succeeded. Runtime output remains streaming, so a
successful long capture is visible to downstream readers as it is produced.
Every successful path, including `--help`, `--version`, `--list`, and
`--probe`, flushes and checks stdout before returning zero; a full filesystem,
broken export target, closed downstream pipe, or other detected write failure
produces a nonzero exit. `SIGPIPE` is converted to a checked `EPIPE`, allowing
PMU and cached sysfs resources to be cleaned up before exit.

CSV exports include `schema_version`, one-based `interval`, measured
`duration_us`, second-resolution `timestamp`, nanosecond-resolution
`timestamp_ns`, and RFC 3339 `timestamp_iso` columns at the front of each row.
JSON carries the same metadata. Consumers should use `duration_us`, rather than
the requested interval, when they need the actual measured window length.

Summary CSV uses a `Scope` identity column containing `SUM`. When multiple
scopes are selected, schema 7 CSV additionally uses `CPU,Package` identity
columns and emits `SUM`, `PKG`, or `CPU` rows. Exact package fields such as
`-s pkg_avg_freq` produce a usable package-only export instead of an empty file.

Detailed JSON/CSV field and structure documentation lives in the
[English](docs/REFERENCE.md#output-contract) and
[Chinese](docs/REFERENCE.zh-CN.md#输出契约) reference.

### Summary mode

```bash
armstat -S
armstat -S -a
```

`-S` enables summary-only output. `-a` enables all supported base column
groups and intentionally does not auto-enable PMU/IPC.
In text/JSON mode, using `-a` or explicitly selecting summary-scope groups via
`-s` prints a `SUM` section when system-scope fields are enabled. Package
aggregation rows appear only when the package group is also enabled (`-s pkg` or
`-a`).

When `--cpu` filtering is active, this automatic mixed `SUM` section is
suppressed to avoid surprising mixed-scope output. Use `-S --cpu ...` when you
want a filtered tracked-CPU summary.

### CPU filtering

```bash
armstat -c 0,1,2-5
```

`--cpu` accepts real Linux CPU IDs and ranges, then applies that list before
runtime sampling starts. A filtered run tracks only online matching CPUs, so
per-CPU rows, summary averages, PMU groups, cpufreq reads, cpuidle refreshes,
and per-CPU Busy/Idle accounting all use the filtered tracked set. Pure
platform/global summary fields remain summary-scope signals.

The parser is strict: invalid tokens such as `bad`, reversed ranges such as
`3-1`, empty tokens such as `0,,2`, and filters that match no online CPU are
reported as startup errors.

### Column selection

```bash
armstat -s all
armstat -s power,temp
armstat -s pkg,core,node,freq,idle
armstat -s LPI-0,LPI-1,Idle%,Busy%
armstat -H temp
```

Both `-s` and `-H` reject unknown column groups or field names as startup
errors. After capability discovery, armstat also rejects a selection that
cannot produce any row in the chosen mode (for example `-S -s cpu` or a sole
dynamic field unavailable on that platform), instead of silently streaming an
empty dataset. Use `--probe` and `--list` to resolve such a selection.

Supported column groups:

- `cpu`
- `pkg`, `package` — per-package/socket aggregation rows
- `core`
- `numa`, `node`
- `freq`
- `idle`
- `power`
- `temp`
- `pmu`
- `sysstat`, `irq`
- `membw`, `mem`
- `ipc`
- `energy`, `joules`
- `all`

`-s` / `-H` also accept exact field names, for example `Idle%`, `Busy%`,
`IOWait%`, `SoftIRQs`, `LPI-0`, or `Power`.
`--list` reports each exact field's scope, machine type, unit, text label, and
JSON key, including `MiB/s` for memory bandwidth and `count/interval` for the
three procstat counters.

Repeated `-s` options form a union. Explicit metric requests `-p`, `-I`, and
`-J` also remain in that union regardless of whether they appear before or
after `-s`; ordinary option ordering cannot silently disable requested PMU,
IPC, or energy output.

### PMU

```bash
armstat -p cycles,instructions
armstat -p cache-misses,branches
armstat -I
```

Built-in PMU names:

- `cycles`
- `instructions`
- `cache-references`
- `cache-misses`
- `branches`
- `branch-misses`
- `mem-access`
- `mem-read` (architectural load-retired event)
- `mem-write` (architectural store-retired event)
- `l1d-cache-refill`, `l1d-cache`
- `l1i-cache-refill`, `l1i-cache`
- `l2d-cache-refill`, `l2d-cache`
- `l3d-cache-refill`, `l3d-cache`

Raw ARM PMU event configs can also be requested as hexadecimal values such as
`0x11`. Unknown named events and lists longer than `MAX_PMU_EVENTS` fail before
sampling starts. If an event is known but perf access is denied or cannot be
opened on the current machine, requested PMU columns remain visible and render
as unavailable instead of reporting fake zeros.

### Helper output

```bash
armstat -l
armstat --probe
```

`-l` prints the built-in column groups, every exact selectable field with its
scope/text label/JSON key, and the PMU event names known to the tool. Stable
field IDs are the least ambiguous tokens to use in scripts.
`--probe` prints a one-shot capability summary for the current platform,
including CPU topology, effective busy-source policy, cpuidle/LPI availability,
available temperature sources and node mask, selected package-power and
memory-bandwidth sysfs paths, candidate counts and ambiguity notes, and a basic
PMU availability probe. The PMU check
opens `cycles` on the first tracked CPU only; it is a cheap capability check,
not proof that every event can open on every CPU. The key-value output reports
`probe_schema_version: 1` so deployment parsers can reject incompatible future
probe contracts explicitly.

### Plotting

Helper plotting scripts are covered in the
[reference](docs/REFERENCE.md#plotting-exports).

### Export Contract

The machine-readable export contract is covered in the
[reference](docs/REFERENCE.md#output-contract).

### Testing

Testing guidance is covered in the
[reference](docs/REFERENCE.md#build-and-validation).

## Columns and Scope

The current field model is scope-aware.

### Summary-scope fields

- `AvgFreq`
- `UncoreFreq`
- idle state residency columns (`LPI-*`, using cpuidle `stateN/name`) when
  cpuidle data exists
- `Idle%`
- `IOWait%`
- `Busy%`
- `Power`
- `Temp0` ... `Temp3` when available
- `Energy`
- `MemBW`
- `CtxSw`
- `IRQs`
- `SoftIRQs`
- `IPC`
- aggregated PMU counters

### CPU-scope fields

- `CPU`
- `Pkg`
- `Core`
- `Node`
- `Freq`
- `Min`
- `Max`
- `Governor`
- `Boost`
- idle state residency columns (`LPI-*`, using cpuidle `stateN/name`) when
  cpuidle data exists
- `Idle%`
- `IOWait%`
- `Busy%`
- `IPC` when requested
- `Temp` derived from NUMA/node temperature
- per-CPU PMU counters when requested

For idle columns specifically:

- `Idle%` / `Busy%` follow the selected busy-source policy (`procstat` or
  `schedstat`, with `task-clock` retained as a compatibility alias for the
  `schedstat` path)
- the sum of visible `LPI-*` columns is intentionally kept close to `Idle%`
- the deepest visible `LPI-*` column may therefore be residual-adjusted rather
  than a raw cpuidle percentage

## Data Sources and Calculations

This section documents the current implementation, including intentional
display-oriented adjustments.

### CPU identity and topology

- **CPU / Pkg / Core / Node**
  - source:
    `cpu_inventory.c` and `topology.c`
  - meaning:
    real Linux CPU ID plus cached package/core/NUMA metadata
  - note:
    CPU rows are sorted by real CPU ID, even though internal arrays use dense
    tracked indices

### Frequency fields

- **Freq**
  - source:
    `/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq`
  - unit:
    MHz
  - formula:
    `Freq = scaling_cur_freq / 1000`

- **Min / Max**
  - source:
    `scaling_min_freq`, `scaling_max_freq`
  - unit:
    MHz
  - formula:
    `Min = scaling_min_freq / 1000`, `Max = scaling_max_freq / 1000`

- **Governor**
  - source:
    `scaling_governor`
  - note:
    refreshed in the slow-changing layer; unavailable values are `-` in text,
    JSON `null`, and an empty CSV cell

- **Boost**
  - source:
    per-CPU `cpufreq/boost`, with fallback to global `cpu/cpufreq/boost`
  - values:
    text/CSV use `1` and `0`; JSON uses `true` and `false`; unavailable values
    are `-` in text, JSON `null`, and an empty CSV cell

- **AvgFreq**
  - per-CPU formula:
    `(prev_cur_freq + cur_freq) / 2`
  - summary formula:
    average of per-CPU interval MHz across valid tracked CPUs
  - note:
    this is an interval-average approximation derived from two samples, not a
    hardware APERF/MPERF style average; a failed read is unavailable and the
    next successful read starts a new averaging baseline

- **UncoreFreq** / `uncore_freq`
  - scope:
    summary only
  - source:
    a unique readable `/sys/class/devfreq/<device>/cur_freq` whose device name
    looks like an uncore/interconnect/fabric source
  - unit:
    MHz
  - formula:
    `uncore_freq = cur_freq_hz / 1000000`
  - note:
    this is a platform-level devfreq reading, not a per-CPU field; if the
    devfreq topology is ambiguous, armstat hides `uncore_freq` instead of
    guessing

### Busy / idle fields

- **Idle%**
  - source:
    selected busy-source policy
  - role:
    authoritative per-CPU idle budget for the interval
  - implementation path:
    `sample_cache.c` captures raw cumulative idle/runtime counters in the
    snapshot, and `aggregator.c` derives interval percentages from the delta
    against the previous snapshot
  - formulas:
    - `procstat`: `/proc/stat` per-CPU `idle`, then
      `idle_delta_us / interval_delta_us * 100`
    - `schedstat`: `100 - Busy%`, where `Busy%` comes from `/proc/schedstat`
      per-CPU runtime
    - `task-clock`: legacy alias for the `schedstat` formula
  - interpretation:
    `Idle%` is the total idle share of the whole sampling window. armstat uses
    it as the reference value that the displayed `LPI-*` breakdown should
    explain. The procstat idle value includes Linux's iowait field.

- **IOWait%** (off by default; enable with `-s IOWait%` or `-s idle`)
  - source:
    `/proc/stat` per-CPU `iowait`
  - implementation path:
    captured as raw cumulative jiffies in `sample_cache.c`, then converted to
    interval percentage in `aggregator.c`
  - formula:
    `iowait_delta_us / interval_delta_us * 100`

- **Busy%**
  - source:
    derived
  - implementation path:
    always computed in `aggregator.c` from the same interval delta used for
    `Idle%`
  - formula:
    `100 - Idle%`
  - note:
    `IOWait%` is a subset of `Idle%`, so it is already excluded from `Busy%`
  - interpretation:
    `Idle% + Busy% = 100`, while `IOWait%` is an advisory breakdown inside the
    idle share, not a third partition bucket

### LPI / cpuidle fields

- **LPI-* labels**
  - source:
    `cpuidle/stateN/name`
  - visibility:
    up to eight columns are exposed (`LPI-0` ... `LPI-7`); deeper kernel
    cpuidle states are not printed as separate columns

- **Raw residency input**
  - source:
    `cpuidle/stateN/time`
  - implementation path:
    `cpuidle.c` reads `stateN/time`, computes raw state deltas, and stores raw
    per-state residency percentages before display adjustment
  - formula:
    `state_delta_us / interval_delta_us * 100`
  - meaning:
    raw cpuidle view of how long the CPU stayed in each idle state during the
    interval

- **How Idle% and raw LPI relate**
  - `Idle%` answers "how idle was this CPU overall in this interval?"
  - raw `LPI-*` answers "how much time did cpuidle account to each state?"
  - on some ARM platforms, short-interval cpuidle state accounting can lag
    behind the overall idle view from the selected busy-source
  - therefore raw `sum(LPI-*)` is not required to equal authoritative `Idle%`

- **Displayed LPI-* semantics**
  - shallower usable states keep their raw cpuidle percentages
  - the deepest usable visible state becomes the residual bucket:
    `Idle% - sum(shallower visible LPI states)`
  - if a deeper state is disabled, unavailable, or beyond the eight-column
    visible limit, the residual is assigned to the deepest remaining usable
    visible state
  - goal:
    keep visible `LPI-*` columns close to authoritative `Idle%`
  - result:
    displayed `LPI-*` is a presentation-friendly decomposition of `Idle%`, not
    always a pure unmodified dump of raw `cpuidle/stateN/time` percentages
  - failure handling:
    incomplete/reset state counters make all visible LPI values for that CPU
    unavailable until the next complete interval

### Power and energy

- **Power**
  - source:
    package-level `power_meter/power1_average`
  - unit:
    mW
  - note:
    the package power reader is platform-specific and maps one uniquely
    discovered package sensor to the `SUM` row; zero or multiple candidates
    leave the field unavailable

- **Interval average power**
  - formula:
    `(prev_power_mw + cur_power_mw) / 2`

- **Energy**
  - formula:
    `interval_avg_power_mw * interval_seconds / 1000`
  - note:
    this is interval energy, not a cumulative counter

### Temperature

- **Temp0 ... Temp3**
  - source:
    `thermal_zoneN/temp`
  - mapping:
    `thermal_zoneN -> TempN -> NUMA/Vdie N`
  - unit:
    Celsius
  - note:
    node IDs may be sparse and readings may be below zero Celsius

- **Temp in CPU rows**
  - formula:
    CPU row uses the temperature of the CPU's NUMA node
  - note:
    this is a NUMA-level summary temperature mapping, not a per-core sensor

### System counters

- **CtxSw**
  - source:
    `/proc/stat` `ctxt`
  - formula:
    `ctxt_now - ctxt_prev`

- **IRQs**
  - source:
    `/proc/stat` `intr`
  - formula:
    `intr_now - intr_prev`

- **SoftIRQs**
  - source:
    `/proc/stat` `softirq`
  - formula:
    `softirq_now - softirq_prev`

All three are interval counts, not normalized per-second rates.

### Memory bandwidth

- **MemBW**
  - source:
    one uniquely discovered platform-dependent raw memory-bandwidth byte
    counter
  - formula:
    `(counter_now - counter_prev) / interval_seconds`
  - unit:
    MiB/s (bytes divided by 1024 squared per second)
  - note:
    if the platform exposes zero or multiple candidate counters, or the unique
    counter is unreadable, `MemBW` is unavailable; a valid interval with no
    transferred bytes is reported as zero

### PMU and IPC

- **PMU event columns**
  - source:
    `perf_event_open()` per tracked CPU
  - model:
    per-CPU perf groups with `time_enabled` / `time_running` scaling
  - displayed value:
    interval delta of the scaled cumulative count
  - validity:
    initial, failed, reset, or unscheduled group reads are unavailable and
    establish a new baseline before the next delta

- **Summary PMU**
  - formula:
    sum of per-CPU scaled PMU counts across tracked CPUs, then interval delta
  - validity:
    unavailable unless every tracked CPU has a complete group read

- **IPC**
  - formula:
    `instructions / cycles`
  - scope:
    available as both summary and per-CPU derived data when those PMU events are
    active and the interval cycle count is nonzero

## Fallback and Degraded Behavior

`armstat` tries to keep the tool usable even when one source is unavailable.

- If `cpuidle` is unavailable or disabled:
  - `Idle%` / `Busy%` still work
  - `LPI-*` columns are hidden
- If PMU access fails:
  - explicitly requested PMU / IPC columns remain visible
  - values are rendered as unavailable rather than silently turning into `0`
- If package power or NUMA temperature sensors are unavailable:
  - the corresponding fields show as unavailable or remain hidden
  - unrelated fields continue to work
- If package power or memory bandwidth has multiple candidate sysfs sources:
  - the metric remains unavailable rather than selecting a nondeterministic
    first match or silently undercounting
  - `--probe` reports the candidate count and an ambiguity note
- If a normally available frequency, Busy/Idle, LPI, power/energy, temperature,
  memory-bandwidth, system-counter, or PMU read fails transiently:
  - text renders `-`, JSON renders `null`, and CSV emits an empty value
  - cumulative sources establish a new baseline on recovery before emitting a
    new interval value, preventing false zeroes and recovery spikes
  - a procstat jiffy delta that cannot be represented safely in microseconds is
    unavailable rather than wrapping into a plausible Busy/Idle percentage
- If CPU topology changes at runtime:
  - inventory, sample cache, cpuidle runtime state, PMU, and topology are rebuilt
  - the next sample becomes a new baseline, so counters are not mixed across
    the hotplug boundary
- If `nohz_full` makes `/proc/stat` noisy on short intervals:
  - the default `auto` busy-source policy prefers `/proc/schedstat` on those CPUs
  - longer intervals still tend to be easier to interpret

## Platform Notes

The current sensor policy assumes:

- package power comes from exactly one `power_meter/power1_average` candidate
- memory bandwidth likewise requires exactly one `mem_bytes_read` candidate
- summary temperatures follow the explicit `thermal-zone-index` policy
  `thermal_zoneN/temp -> TempN -> NUMA/Vdie N`

For example:

- a 1-socket / 2-NUMA system typically shows `Temp0` and `Temp1`
- a 2-socket / 4-NUMA system typically shows `Temp0` through `Temp3`

If your platform exposes a different sensor topology, either set
`ARMSTAT_TEMP_POLICY=none` to suppress summary `TempN` discovery or update
`power_sensor.c` with a different summary-temperature policy.

## Known Limitations

- There are not yet dedicated core aggregate rows like mature `turbostat`;
  summary, per-package, and per-CPU rows are implemented.
- Per-core power is not implemented.
- CPU-row temperature is a NUMA/die temperature mapping, not a per-core sensor.
- CPU IDs above 1023 cannot be represented by the current fixed-size sampling
  arrays; armstat warns and continues with representable online CPUs.
- PMU scaling depends on kernel perf group semantics and still needs validation
  on real target hardware.
- PMU access may fail without sufficient privileges.

## Debugging

Use debug mode to inspect initialization and runtime behavior:

```bash
armstat -d -i 1 -n 2
```

Useful things to verify:

- which busy-source policy is effective, and whether cpuidle/LPI is available
- tracked CPU inventory
- PMU initialization success/failure
- discovered power and temperature sensors

## See Also

- [turbostat(8)](https://man7.org/linux/man-pages/man8/turbostat.8.html)
- [perf(1)](https://man7.org/linux/man-pages/man1/perf.1.html)

## License

GPL-2.0. See [COPYING](COPYING) for the complete license text.
