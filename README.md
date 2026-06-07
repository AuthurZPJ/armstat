# armstat
<p align="center">
  <b>English</b> | <a href="[./README.zh-CN.md]">简体中文</a>
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
time-series charts without manual data wrangling. See `EXPORTS.md` and
`PLOTTING.md`.

## Current Output Model

`armstat` currently uses a `SUM + CPU rows` model:

- Default text mode prints one package-row per socket, then one row per tracked CPU
- `-S` prints a single `SUM` row per interval
- `-a` enables all supported base column groups including per-package aggregation
- JSON writes an array of interval objects
- CSV writes either CPU rows or summary rows

This is intentionally `turbostat`-like, but it is not a byte-for-byte clone of
the x86 tool. In particular, ARM platforms often do not expose a uniform
hardware power/thermal/idle model comparable to x86 MSR/RAPL/TSC.

## Mental Model

The easiest way to understand one visible `armstat` line is:

1. establish a baseline sample
2. wait one full interval
3. collect a new raw snapshot
4. derive interval deltas and percentages
5. format those derived values as either `SUM`, CPU rows, or both

That means the first visible line is always "one complete interval later", not
"instantaneous state at program start".

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
  spent in iowait accounting
- Per-state idle residency and wakeup columns use cpuidle `stateN/name`
  labels such as `LPI-0`, `LPI-1`, ..., and `LPI-0_wake` (wakeups/s)
- cpuidle is used for split `LPI-*` residency only
- Per-state idle columns are hidden when cpuidle data is unavailable
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

### Power and temperature

The current implementation is optimized for platforms with:

- package power from `hwmon` `name=power_meter`, using `power1_average`
- summary temperatures from `thermal_zoneN/temp`, where `N` maps to NUMA/Vdie `N`
  under the current `thermal-zone-index` summary-temp policy

That means:

- `Power` is a `SUM`-scope field and is reported in mW
- `Temp` in CPU rows is derived from the CPU's NUMA node temperature and is reported in C
- `Temp0` ... `Temp3` are summary fields shown for discovered NUMA/Vdie zones
  and are reported in C
- per-core power and per-core temperature are currently not exposed

The summary-temperature policy is explicit rather than implicit:

- default policy: `thermal-zone-index`
- behavior: `thermal_zoneN/temp -> TempN -> NUMA/Vdie N`
- override: `ARMSTAT_TEMP_POLICY=none` disables summary `TempN` discovery

`--probe` reports the effective `summary_temp_policy` so platform assumptions
are visible at runtime.

### PMU

PMU support uses `perf_event_open()`:

- counters are opened per tracked CPU
- events are grouped per CPU
- group reads include `time_enabled` and `time_running`
- multiplexed counters are scaled before interval deltas are derived
- `-I` enables `cycles,instructions` and adds IPC columns
- `-p ...` enables PMU counters without implicitly enabling IPC
- unknown PMU event names and event lists longer than `MAX_PMU_EVENTS` fail
  immediately; perf permission/open failures still degrade to visible
  unavailable values for requested columns
- PMU file descriptor use scales with the filtered tracked CPU count, so
  `--cpu` is the primary way to reduce PMU fd pressure on large systems

PMU monitoring usually requires root or a permissive
`/proc/sys/kernel/perf_event_paranoid` setting.

If PMU or IPC columns are explicitly requested (for example via `-p`, `-I`,
`-s pmu`, or `-s ipc`) but no `-p` event list is
provided, armstat defaults to `cycles,instructions`.

### nohz_full and Busy/Idle

`nohz_full` CPUs can make short-interval `/proc/stat` busy/idle percentages
look erratic. For that reason, the default `auto` busy-source policy prefers
`/proc/schedstat` runtime accounting on CPUs listed in
`/sys/devices/system/cpu/nohz_full`, while continuing to use `/proc/stat` on
ordinary CPUs.

## Architecture

The implementation is split into clear layers:

```text
armstat.c              main loop and module lifecycle
armstat_cli.c          command-line parsing and column selection
collector.c            orchestrates one interval of collection
sample_cache.c         memory pools + fast-path sampling
idle_backend.c         busy-source policy helpers (/proc/stat vs /proc/schedstat)
aggregator.c           delta/interval calculations
formatter.c            output formatting facade
formatter_record.c     interval_record builder and field table
formatter_text.c       text output
formatter_machine.c    JSON/CSV output
cpu_inventory.c        single source of truth for present/online/tracked CPUs
topology.c             package/core/NUMA metadata
power_sensor.c         platform sensor discovery
power_interval.c       interval average power and energy
membw.c                memory bandwidth counter tracking
pmu.c                  perf-based PMU collection
cpufreq.c              CPU frequency and governor
cpuidle.c              cpuidle state residency (LPI-*)
sysstat.c              /proc/stat and /proc/schedstat readers
```

## Optimization Strategy

The current implementation is optimized around "cheap enough for large ARM
systems" rather than "read everything on every interval".

### 1. Three sampling layers

- **Static / rebuild layer**:
  CPU inventory, topology, sensor discovery, cpuidle state names, PMU event
  metadata
- **Slow-changing layer**:
  CPU min/max frequency, governor, boost, sensor capability flags, cpuidle
  `disable` state
- **Per-interval fast path**:
  current frequency, `/proc/stat` deltas, package power, NUMA temperatures,
  PMU counters, cpuidle `stateN/time`

This keeps the expensive "what exists on this platform?" work out of the hot
path.

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
interval deltas are derived.

### 6. Two-stage formatting

Formatting is split into:

1. `formatter_record.c`: build a stable `interval_record`
2. `formatter_text.c` / `formatter_machine.c`: serialize that record

This keeps formatting logic consistent across text, JSON, and CSV without
recomputing metrics in serializers.

## Build

```bash
cd tools/power/armstat
make
```

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

CSV exports include `schema_version`, `interval`, `timestamp`, and
`timestamp_iso` columns at the front of each row so downstream tools can align
samples in time and identify the current export contract.

Detailed JSON/CSV field and structure documentation lives in `EXPORTS.md`
(English) and `EXPORTS.zh-CN.md` (Chinese).

### Summary mode

```bash
armstat -S
armstat -S -a
```

`-S` enables summary-only output. `-a` enables all supported base column
groups and intentionally does not auto-enable PMU/IPC.
In text/JSON mode, using `-a` or explicitly selecting summary-scope groups via
`-s` prints a `SUM` section in addition to package and CPU rows when system-scope
or package-scope fields are enabled.

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

`-s` / `-H` also accept exact field names, for example `Idle%`, `Busy%`,
`IOWait%`, `SoftIRQs`, `LPI-0`, or `Power`.

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
- `mem-read`
- `mem-write`
- `l1d-cache`
- `l1d-cache-refill`
- `l1i-cache`
- `l1i-cache-refill`
- `l2d-cache`
- `l2d-cache-refill`
- `l3d-cache`
- `l3d-cache-refill`

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

`-l` prints the built-in column groups and PMU event names known to the tool.
`--probe` prints a one-shot capability summary for the current platform,
including CPU topology, effective busy-source policy, cpuidle/LPI availability,
available temperature sources, memory-bandwidth support, and a basic PMU
availability probe.

### Plotting

Helper plotting scripts are documented separately in `PLOTTING.md`
(English) and `PLOTTING.zh-CN.md` (Chinese).

### Export Contract

The machine-readable export contract is documented separately in
`EXPORTS.md` (English) and `EXPORTS.zh-CN.md` (Chinese).

### Testing

Testing guidance is documented separately in `TESTING.md` (English) and
`TESTING.zh-CN.md` (Chinese).

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
    refreshed in the slow-changing layer

- **Boost**
  - source:
    per-CPU `cpufreq/boost`, with fallback to global `cpu/cpufreq/boost`
  - values:
    `1`, `0`, or `-` when unavailable

- **AvgFreq**
  - per-CPU formula:
    `(prev_cur_freq + cur_freq) / 2`
  - summary formula:
    average of per-CPU interval MHz across valid tracked CPUs
  - note:
    this is an interval-average approximation derived from two samples, not a
    hardware APERF/MPERF style average

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
    explain.

- **IOWait%**
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
    `IOWait%` is reported separately but is not subtracted from `Busy%` in the
    current model
  - interpretation:
    `Idle% + Busy% = 100`, while `IOWait%` is an additional advisory breakdown
    from `/proc/stat`, not a third partition bucket

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

### Power and energy

- **Power**
  - source:
    package-level `power_meter/power1_average`
  - unit:
    mW
  - note:
    the package power reader is platform-specific and currently maps one
    package sensor to the `SUM` row

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
    platform-dependent raw memory-bandwidth byte counter
  - formula:
    `(counter_now - counter_prev) / interval_seconds`
  - unit:
    MB/s
  - note:
    if the platform does not expose a usable raw counter, `MemBW` may remain
    unavailable or zero

### PMU and IPC

- **PMU event columns**
  - source:
    `perf_event_open()` per tracked CPU
  - model:
    per-CPU perf groups with `time_enabled` / `time_running` scaling
  - displayed value:
    interval delta of the scaled cumulative count

- **Summary PMU**
  - formula:
    sum of per-CPU scaled PMU counts across tracked CPUs, then interval delta

- **IPC**
  - formula:
    `instructions / cycles`
  - scope:
    available as both summary and per-CPU derived data when those PMU events are
    active

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
- If CPU topology changes at runtime:
  - inventory, sample cache, cpuidle runtime state, PMU, and topology are rebuilt
  - the next sample becomes a new baseline, so counters are not mixed across
    the hotplug boundary
- If `nohz_full` makes `/proc/stat` noisy on short intervals:
  - the default `auto` busy-source policy prefers `/proc/schedstat` on those CPUs
  - longer intervals still tend to be easier to interpret

## Platform Notes

The current sensor policy assumes:

- package power comes from `power_meter/power1_average`
- summary temperatures follow the explicit `thermal-zone-index` policy
  `thermal_zoneN/temp -> TempN -> NUMA/Vdie N`

For example:

- a 1-socket / 2-NUMA system typically shows `Temp0` and `Temp1`
- a 2-socket / 4-NUMA system typically shows `Temp0` through `Temp3`

If your platform exposes a different sensor topology, either set
`ARMSTAT_TEMP_POLICY=none` to suppress summary `TempN` discovery or update
`power_sensor.c` with a different summary-temperature policy.

## Known Limitations

- The output model is still `SUM + CPU rows`; there are not yet dedicated
  package/core aggregate rows like mature `turbostat`.
- Per-core power is not implemented.
- CPU-row temperature is a NUMA/die temperature mapping, not a per-core sensor.
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

GPL-2.0
