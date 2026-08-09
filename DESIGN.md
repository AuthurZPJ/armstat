# ARMSTAT Design

<p align="center">
  <a href="README.md">← Back to README</a> |
  <a href="TESTING.md">Testing</a> |
  <a href="EXPORTS.md">Exports</a> |
  <a href="PLOTTING.md">Plotting</a>
</p>

This document describes the current implementation, not an aspirational one.
If behavior differs from older notes, the code wins and this file should be
updated.

## Goals

`armstat` is intended to be an ARM64 server monitor in the style of
`turbostat`, with emphasis on:

- interval-based statistics
- stable CPU identity under sparse CPU numbering
- low enough sampling overhead for large ARM systems
- clear separation between collection, aggregation, and formatting
- platform-specific sensor adaptation without contaminating the generic layers

## Non-goals

Current `armstat` does not try to fully replicate x86-specific `turbostat`
hardware semantics such as MSR/TSC/RAPL style power and clock domains.

## Current Layering

```text
armstat_cli.c (CLI parsing)
  |
armstat.c (main loop + module lifecycle)
  -> collector.c
       -> cpu_inventory
       -> sample_cache
       -> idle_backend (policy helpers only)
       -> cpufreq / cpuidle / power / pmu / sysstat readers
  -> aggregator.c
  -> columns.c (column visibility + field descriptor table)
  -> formatter_record.c (interval_record builder; value getters)
  -> formatter_text.c / formatter_machine.c (serializers, dispatched by armstat.c)
```

### armstat.c and armstat_cli.c

armstat_cli.c responsibilities:

- parse CLI options
- apply column visibility selections (-s/-H/-a)
- print help and version text

armstat.c responsibilities:

- initialize modules in dependency order
- establish one baseline sample
- run the interval loop
- handle signals and priority boost
- clean up on exit
- --probe one-shot capability dump

Important behavior:

- the first visible sample is emitted after one full interval
- `-S` is summary-only
- `-a` enables all supported column groups
- `-D` is equivalent to one iteration

### collector.c

Responsibilities:

- own the unified interval timestamp
- detect CPU inventory changes
- rebuild hotplug-dependent state
- coordinate per-interval sampling

It should remain an orchestrator, not a place where detailed parsing logic
accumulates.

### sample_cache.c

Responsibilities:

- provide per-interval memory pools
- implement slow-changing cache refreshes
- read fast-changing raw values for one interval

Sampling layers:

1. Static / hotplug rebuild
2. Slow refresh (~5 seconds)
3. Per-interval sampling

Current optimization strategy inside the cache layer:

- refresh slow-changing CPU data with a rolling cursor and a budget, not with
  periodic full-machine sweeps
- keep `/proc/stat` parsing outside repeated per-metric code paths
- only refresh cpuidle split-state data when `LPI-*` fields are visible
- only refresh package power / temperature when a visible field depends on them
- best-effort elevate the armstat process priority before interval sampling to
  reduce self-induced scheduling jitter; failure is non-fatal

Slow refresh mechanics in `sample_cache.c`:

- `slow_init()` allocates the slow-changing caches (`min/max/governor/boost`)
  and resets `slow_cursor`
- the first interval after init or rebuild calls `slow_update_all()`, giving
  the cache a complete baseline immediately
- later intervals call `slow_update_budgeted()`
- `slow_budget_for_interval()` computes:
  `budget ~= tracked_cpus * delta_us / target_sweep_us`
  where `target_sweep_us` is derived from `SLOW_TARGET_SWEEP_MS_DEFAULT`
- the budget is clamped between `SLOW_MIN_CPU_BUDGET` and
  `SLOW_MAX_CPU_BUDGET`
- the sweep window can be customized via the `ARMSTAT_SLOW_SWEEP_MS`
  environment variable (range 100-60000 ms; default 5000)
- `slow_cursor` identifies the next tracked CPU to refresh, so each interval
  updates only a contiguous slice and then advances the cursor
- the cpuidle `disable` cache is refreshed with the same budgeted model via
  `refresh_idle_state_disable_cache_budgeted()`
- `maybe_run_slow_refresh()` is intentionally called after the fast-path sample
  so that interval-critical data is captured before any low-frequency
  housekeeping work

This design trades immediate refresh of every slow-changing field for bounded,
predictable overhead on large systems.

It also owns the raw authoritative Busy/Idle inputs for each interval:

- `/proc/stat` `idle`
- `/proc/stat` `iowait`
- `/proc/schedstat` runtime for CPUs whose busy-source policy selects it

Those values are stored as cumulative raw counters in `sys_snapshot`, not
converted to percentages in the collector layer.

### idle_backend.c

Responsibilities:

- keep busy-source policy in one place
- parse `/sys/devices/system/cpu/nohz_full`
- answer whether a given CPU should use `/proc/stat` or `/proc/schedstat`

Current policy:

- select the Busy/Idle authority via a busy-source policy
- default `auto` mode uses `/proc/stat` on ordinary CPUs and prefers
  `/proc/schedstat` runtime accounting on CPUs listed in
  `/sys/devices/system/cpu/nohz_full`
- `IOWait%` continues to come from `/proc/stat`
- use cpuidle only for split `LPI-*` residency when cpuidle is available

This file no longer owns a runtime backend object or a separate hidden delta
timeline. Busy/Idle percentages are derived later from raw counters stored in
the snapshot.

### aggregator.c

Responsibilities:

- derive interval deltas from raw counters
- compute average MHz, busy/idle, power, energy, membw, PMU, IPC
- keep previous raw baselines for the next interval

The aggregator must not do sysfs or procfs I/O.

### Formatter output stack

Responsibilities:

- convert `sys_snapshot + interval_stats` into an `interval_record`
- serialize that intermediate model to text, JSON, or CSV

The formatter stack is intentionally two-stage:

1. `columns.c` owns the column-visibility flags (`show_*`), the idle-state and
   summary-temp series visibility + override bitmasks, and the field descriptor
   table (`all_fields[]`) that ties field ids to their group, scope, series,
   enabled flag, and value getter. CLI parsing writes visibility through the
   `enable_*()` setters; sample_cache reads it for demand-driven sampling.
2. `formatter_record.c` builds a stable intermediate record — the value
   getters referenced by the field table live here, next to the record model.
3. serializers consume the record without knowing sampling details or column
   visibility internals.

## CPU Identity Model

This is one of the most important design decisions.

Two identities exist at the same time:

- `cpu_id`: the real Linux CPU ID
- `tracked_idx`: the dense internal array index used by pools

Rules:

- external semantics always use `cpu_id`
- internal arrays always use `tracked_idx`
- topology lookups always take `cpu_id`
- CPU filters are expressed in real CPU IDs
- `--cpu` is applied before runtime sampling: only online CPUs matching the
  filter become tracked CPUs, so PMU groups, cpufreq/cpuidle reads, per-CPU
  Busy/Idle inputs, and tracked-CPU averages are based on the filtered set
- invalid filter tokens, reversed ranges, empty tokens, and filters that match
  no online CPU are startup errors

This prevents sparse numbering and hotplug from corrupting topology or output.

## CPU Inventory

`cpu_inventory` now owns the single source of truth for:

- present CPUs
- online CPUs
- tracked CPUs
- topology attributes cached per CPU

It also exposes the compatibility helpers used throughout the collector and
formatter layers:

- `get_cpu_id_by_tracked_idx()`
- `get_tracked_cpu_count()`
- change detection via `check_and_rebuild_inventory()`

Hotplug detection is based on actual membership changes, not only CPU count.

## Hotplug Rebuild Chain

When CPU membership changes:

1. rebuild CPU inventory
2. rebuild sample cache
3. rebuild cpuidle runtime state
4. rebuild PMU if active
5. rebuild topology
6. reset aggregator
7. treat the current sample as a fresh baseline

This avoids mixing pre-hotplug and post-hotplug counters into one interval.

## Data Model

### Raw snapshot (`sys_snapshot`)

The snapshot is owned by the collector and exposed to consumers through
accessor functions (`sys_snapshot_get_effective_cpu_count`,
`sys_snapshot_get_interval_delta_us`, `sys_snapshot_get_cpu_truncated`,
`sys_snapshot_get_freqs`, `sys_snapshot_get_counters`). Multi-consumer
fields are read through these getters so a layout change does not ripple
across aggregator, formatter, and main loop. Single-consumer fields remain
accessed directly for now; a future step can make the struct fully opaque.

Contains:

- CPU counts and truncation metadata
- raw per-CPU frequency and optional per-state idle data
- package power
- NUMA temperature array
- raw PMU counters
- raw system counters (`struct raw_counters`, also independently instantiated
  by the aggregator for `prev_counters`)
- unified interval delta in microseconds
- per-tracked-CPU schedstat validity flags, allowing per-CPU fallback to
  `/proc/stat` when `/proc/schedstat` lacks data for that CPU

### Interval stats (`interval_stats`)

Contains:

- interval-derived system statistics
- per-CPU MHz / busy / idle / iowait
- interval average power and energy
- membw
- PMU deltas and IPC

### Intermediate record (`interval_record`)

The record is fully materialized per interval: it owns every per-interval
dynamic value, so serializers never dereference the raw snapshot or the
interval stats after `build_interval_record()` returns.

Contains:

- interval metadata
- summary data (`summary_data`)
- owned per-CPU rows (`cpu_rows`) with the freq snapshot, busy/idle/iowait, IPC,
  per-idle-state residency and wakeups, per-CPU PMU counters, and CPU temperature
- owned per-package aggregation rows (`packages`)
- owned summary idle-state residency (`summary_idle_state_pct`) and NUMA
  temperatures (`numa_temps`)

Static identity fields (package, core, NUMA node) are still looked up lazily at
output time from the topology caches via the tracked CPU id.

This allows one field table to drive text/JSON/CSV consistently.

## Output Model

Current output scope is split into:

- system-scope fields
- CPU-scope fields

Current text layout is:

- default mode: per-CPU rows only
- `-a` mode: `SUM` + per-package aggregation rows + CPU rows
- summary mode: one SUM row only

In mixed-scope output (`-a`), each section keeps its own header line directly
above its rows, with a blank line separating the SUM, Pkg, and CPU sections, so
the three tables stay visually distinct instead of running together.

Package rows aggregate per-CPU MHz, Idle%, Busy%, and IOWait% by socket. They are
emitted only when the package column group is explicitly enabled (`-a` or
`-s package`); the default output stays per-CPU only. Core-level aggregation is
not yet implemented.

Important summary semantics:

- summary counters such as power, membw, PMU, and system counters are built at
  summary scope
- summary percentages such as `Idle%`, `Busy%`, and `IOWait%` are summary-level
  percentages derived from aggregated interval stats
- summary `LPI-*` values are averages of the displayed per-CPU `LPI-*` values,
  not a second whole-machine residual pass
- when `--cpu` filtering is active, tracked-CPU-derived summary fields are
  based on the filtered tracked CPU set, while platform/global fields keep
  their natural summary scope
- when `--cpu` filtering is active, the automatic mixed `SUM` section is
  suppressed so CPU rows are not shown beside a surprising implicit summary
  section
- per-package aggregation rows are also suppressed when `--cpu` is active, since
  they would aggregate across CPUs outside the filter

## Idle Model

Busy/idle uses the selected busy-source policy as the authoritative source;
cpuidle is used for split LPI residency only.

Default `auto` mode behaves as follows:

- ordinary CPUs use `/proc/stat`
- `nohz_full` CPUs prefer `/proc/schedstat` runtime accounting
- if schedstat cannot be read for a CPU, that CPU falls back to `/proc/stat`

The `task-clock` mode is kept as a user-visible compatibility knob, but it now
uses the same implementation as `schedstat`. CPU-wide perf `task-clock`
produced inverted Busy/Idle results on the target ARM servers, so it is no
longer treated as the authoritative Busy/Idle source.

When cpuidle is available:

- total idle time still feeds `Idle%` / `Busy%`
- iowait time separately feeds `IOWait%`
- per-state residency columns are derived from cpuidle state deltas
- per-state wakeup columns are derived from cpuidle usage counters
  (entries per second)
- state labels come from cpuidle `stateN/name` (for example `LPI-0`, `LPI-1`)
- only existing state columns are shown
- up to eight state columns are visible (`LPI-0` ... `LPI-7`)
- the deepest visible usable state is treated as a residual bucket so the
  displayed `LPI-*` columns sum to authoritative `Idle%`
- if a deeper state is disabled, unavailable, or beyond the visible column
  limit, the residual is assigned to the deepest remaining visible usable state

This means:

- `Busy%` is always `100 - Idle%`
- `IOWait%` is reported independently from `/proc/stat`
- shallower visible `LPI-*` states preserve raw cpuidle residency as much as
  possible
- the deepest visible `LPI-*` value is display-adjusted when needed, rather
  than always being a pure raw cpuidle percentage

When cpuidle is not available:

- per-state columns are hidden
- `Idle%` / `Busy%` still work via `/proc/stat`

## Power and Thermal Model

The current platform-oriented model is:

- package power from `power_meter/power1_average`
- summary temperatures from `thermal_zoneN/temp`, using the explicit
  `thermal-zone-index` policy where `N` maps directly to NUMA/Vdie `N`

Consequences:

- `Power` is a summary/package field and is reported in mW
- interval average power uses a trapezoid over previous and current package
  readings: `(prev + current) / 2`
- `Energy` is interval energy derived from interval average power, not a
  cumulative lifetime counter
- CPU-row temperature is derived from the CPU's NUMA node
- summary temperature fields are `Temp0` through `Temp3` as available and are reported in C
- per-core power is intentionally not exposed yet

This logic lives in `power_sensor.c` so that future platforms can replace the
sensor policy without rewriting collector/formatter code. The current policy is
reported by `--probe` as `summary_temp_policy`, and can be disabled with
`ARMSTAT_TEMP_POLICY=none` when the platform does not follow the direct
`thermal_zoneN -> TempN -> NUMA/Vdie N` mapping.

## PMU Model

The PMU implementation is intentionally closer to `turbostat` than earlier
versions:

- counters are opened per tracked CPU
- events on the same CPU are opened as one perf group
- reads use `PERF_FORMAT_GROUP`
- `time_enabled` / `time_running` are collected
- multiplexed intervals are scaled before they are accumulated
- aggregator derives per-CPU and summary deltas from those scaled cumulative
  values
- event names are validated before sampling starts; unknown named events and
  lists longer than `MAX_PMU_EVENTS` are hard CLI errors
- known events that cannot be opened because of perf permissions or runtime
  platform limits degrade to unavailable values in requested columns

This means PMU values are interval deltas of a scaled cumulative counter model,
not direct one-shot raw reads.

Summary:

- default CPU-mode output shows per-CPU PMU values
- summary mode shows aggregated PMU values
- IPC is available as both summary-scope and CPU-scope derived data

Known caveat:

- PMU behavior still needs validation on real target systems, especially under
  heavy multiplexing

## JSON and CSV

JSON and CSV are driven from the same field table as text output.

Current behavior:

- JSON writes an array of interval objects
- default mode emits `cpus: [...]`
- summary mode emits `summary: {...}`
- CSV writes one header row followed by CPU rows or summary rows

The serializers should not encode sampling assumptions; those belong in the
builder and field getters.

## Metric Sources and Formulas

This is the implementation-facing summary of "where each number comes from".

- `Freq`
  - source: `scaling_cur_freq`
  - formula: `cur_freq_khz / 1000`
- `Min` / `Max`
  - source: `scaling_min_freq`, `scaling_max_freq`
  - formula: `khz / 1000`
- `AvgFreq`
  - formula: `(prev_cur_freq + cur_freq) / 2`, then averaged across tracked CPUs
- `UncoreFreq` / `uncore_freq`
  - source: a unique readable `/sys/class/devfreq/<device>/cur_freq` whose
    device name looks like an uncore/interconnect/fabric source
  - formula: `cur_freq_hz / 1000000`
  - scope: summary only
- `Idle%`
  - source: selected busy-source policy
  - role: authoritative total idle budget for the interval
  - implementation path:
    raw cumulative counters are captured in `sample_cache.c` and converted to
    interval percentages in `aggregator.c`
  - formulas:
    - `procstat`: `/proc/stat idle`, then `idle_delta_us / interval_delta_us * 100`
    - `schedstat`: `100 - Busy%`, where `Busy%` comes from `/proc/schedstat`
      per-CPU runtime
    - `task-clock`: legacy alias for the `schedstat` formula
- `IOWait%` (off by default; enable with `-s iowait` or `-s IOWait%`)
  - source: `/proc/stat iowait`
  - implementation path:
    raw cumulative jiffies are captured in `sample_cache.c`; the interval
    percentage is derived in `aggregator.c`
  - formula: `iowait_delta_us / interval_delta_us * 100`
- `Busy%`
  - implementation path:
    derived in `aggregator.c` from the same per-CPU interval deltas used for
    `Idle%`
  - formulas:
    - `procstat`: `100 - Idle%`
    - `schedstat`: `sched_runtime_delta_ns / wall_clock_delta_ns * 100`
    - `task-clock`: legacy alias for the `schedstat` formula
  - note: `IOWait%` is tracked independently and is not subtracted from `Busy%`
- `LPI-*`
  - source: `cpuidle/stateN/time`
  - implementation path:
    `cpuidle.c` computes raw per-state residency percentages first; the
    formatter layer then applies the residual display rule
  - raw formula: `state_delta_us / interval_delta_us * 100`
  - relationship to `Idle%`:
    raw cpuidle residency is an idle-state breakdown input, not the
    authoritative total idle budget
  - visibility:
    up to eight columns are exposed (`LPI-0` ... `LPI-7`)
  - display rule: deepest usable visible state becomes the residual bucket so
    visible `LPI-*` stays close to `Idle%`
  - implication:
    displayed `LPI-*` is optimized for explaining authoritative `Idle%`, not
    for preserving raw cpuidle percentages unchanged
- `Power`
  - source: `power_meter/power1_average`
- `Energy`
  - formula: `interval_avg_power_mw * interval_seconds / 1000`
- `MemBW`
  - source: platform-specific raw byte counter
  - formula: `(counter_now - counter_prev) / interval_seconds`
- `CtxSw`
  - source: `/proc/stat ctxt`
  - formula: `ctxt_now - ctxt_prev`
- `IRQs`
  - source: `/proc/stat intr`
  - formula: `intr_now - intr_prev`
- `SoftIRQs`
  - source: `/proc/stat softirq`
  - formula: `softirq_now - softirq_prev`
- `IPC`
  - formula: `instructions / cycles`

## Degraded Mode Rules

The implementation is intentionally explicit about partial capability:

- if `cpuidle` is unavailable, `Idle%` / `Busy%` still work and `LPI-*` fields
  disappear
- if PMU cannot be initialized, requested PMU / IPC fields remain part of the
  output model but render as unavailable instead of pretending to be zero
- if power or temperature sensors are missing, only the dependent fields are
  unavailable; the rest of the interval model remains valid
- after hotplug rebuild, the next collection is treated as a new baseline so
  deltas do not cross the rebuild boundary

## Current Intentional Limitations

- no core aggregate rows yet (per-package aggregation is implemented)
- no per-core power model
- CPU-row temperature is NUMA-derived, not core-local
- ARM hardware semantics are platform-dependent; not every x86 `turbostat`
  column has an ARM equivalent

## ARM Platform Adaptation

This section documents the design decisions that are specifically driven by
ARM server platform characteristics and Linux kernel interfaces. Understanding
these is essential when extending armstat to new platforms or troubleshooting
unexpected output.

### Why dual-source Busy/Idle

ARM servers commonly use `nohz_full` to stop the scheduling tick on selected
CPUs. When the tick is stopped, `/proc/stat` idle counters update at coarse
(possibly multi-second) granularity that can make short-interval `Idle%`
appear erratic.

`/proc/schedstat` runtime accounting is maintained by the scheduler regardless
of tick state and provides sub-jiffy busy accounting for `nohz_full` CPUs.
This is the motivation for the default `auto` busy-source policy:

- ordinary CPUs use `/proc/stat` (familiar, well-tested)
- `nohz_full` CPUs prefer `/proc/schedstat` (tick-independent)

The `schedstat` path computes `Busy%` from runtime delta and derives
`Idle% = 100 - Busy%`. Because schedstat runtime can occasionally exceed wall
clock on `nohz_full` CPUs due to coarse tick-granularity accounting, the
aggregator clamps `busy_us` to `delta_us` and treats the remainder as idle.
This clamping is intentional and produces correct long-term averages; it
should not be removed unless the kernel's schedstat granularity changes.

### Why LPI-* residual adjustment

ARM cpuidle `stateN/time` counters only advance when a CPU **exits** an idle
state. A CPU that enters its deepest idle state at the beginning of an interval
and never exits during that interval will show **zero delta** in cpuidle for
that state, even though the CPU was idle for the entire interval.

`/proc/stat`, by contrast, correctly accounts for this time as idle.

The residual adjustment in the formatter bridges this gap:

- **shallow states** (frequent entry/exit): keep raw cpuidle residency
  percentages, which are reliable because the counters are updated often
- **deepest visible usable state**: absorbs the remaining `Idle%` not
  accounted for by shallower visible states, effectively capturing the
  "continuous residency without exit" time

This produces visible `LPI-*` columns that sum to the authoritative `Idle%`
while preserving the ARM-specific split-idle information where cpuidle is
most reliable. The formatter exposes at most eight LPI columns. The residual
goes to the deepest **visible usable** state, and if a deeper state is disabled
(`stateN/disable = 1`), unavailable, or beyond that visible limit, the residual
shifts to the deepest remaining visible usable state.

Without this adjustment, `sum(LPI-*)` would systematically undercount `Idle%`
on short sampling intervals, making the split-idle columns misleading.

### Sensor model assumptions

ARM platforms expose power and temperature sensors through `sysfs` / `hwmon`
with platform-specific naming conventions. armstat makes the following
assumptions, which hold on many ARM server platforms but are **not kernel
guarantees**:

**Power**:
- Looks for an `hwmon` device with `name = power_meter`
- Reads `power1_average` for package-level power
- If no `power_meter` device exists, power silently degrades to 0

**Temperature**:
- Default `thermal-zone-index` policy maps `thermal_zoneN/temp` directly to
  NUMA/vdie `N` (i.e., thermal zone index = NUMA node index)
- CPU-row temperature is derived from the CPU's NUMA node temperature
- This mapping is a platform convention, not a kernel guarantee: thermal zone
  numbering depends on probe order and is not inherently linked to NUMA topology
- If the mapping is incorrect on a platform, set `ARMSTAT_TEMP_POLICY=none` to
  disable summary `TempN` discovery, or use `--probe` to inspect the actual
  mapping

**Uncore/DevFreq**:
- Scans `/sys/class/devfreq/` for a device whose name suggests an
  uncore/interconnect/fabric clock source
- Falls back silently if no such device is found

All sensor policies are isolated in `power_sensor.c` so that future platforms
can swap the discovery logic without touching the collector/formatter pipeline.

### Shared sysfs I/O primitives

All sysfs/procfs single-value reads and cached-fd reads go through
`sysfs_util.c` (`sysfs_read_int_checked`, `sysfs_read_ull_checked`,
`sysfs_read_str`, `sysfs_path_exists`, `fd_read_ull_checked`). This
consolidates the previously duplicated `fopen`/`fscanf`/`fclose` and
`lseek`/`read`/`strtoull` patterns that were copy-pasted across `topology.c`,
`power_sensor.c`, `cpufreq.c`, and `cpuidle.c` with subtly different error
conventions. All numeric readers use the checked convention (return 0 on
success, -1 on failure, value via out-param) so callers can choose their own
error sentinel.

### File descriptor budget

armstat keeps file descriptors open across intervals for performance:

| subsystem | open fds | cap |
|-----------|----------|-----|
| cpuidle  | 1 per CPU × per state | ≤ 32 (hard cap) |
| sysstat  | 2 (`/proc/stat`, `/proc/schedstat`) | 2 |
| cpufreq  | 1 per CPU (`scaling_cur_freq`) | ≤ 16 (hard cap, fallback to slow path) |
| PMU      | 1 group fd per tracked CPU | no explicit cap |

On a machine with 256 tracked CPUs and 3 PMU events, the total can exceed 500
file descriptors. Combined with cpufreq fds, this pushes against the typical
default `ulimit -n` of 1024. If PMU initialization fails with `EMFILE`, either
raise `ulimit -n` or reduce the tracked CPU count with `--cpu`. Because
`--cpu` is now applied before sampling, it reduces both PMU group fds and the
other per-tracked-CPU readers.

The cpuidle fd cache is intentionally capped at 32 to prevent split-LPI
reporting from consuming the entire process fd budget. PMU fds are not capped
because PMU is opt-in and the user who enables PMU on a large machine
implicitly accepts the fd cost.

### MAX_CPUS = 1024

`MAX_CPUS` is a compile-time constant in `collector.h`. It sets the maximum
number of simultaneously tracked CPUs. 1024 was chosen to exceed the largest
single-system ARM server available at the time (~512 cores), with headroom for
SMT and growth. If a system exceeds this limit, armstat caps at 1024, prints a
warning, and continues with the first 1024 CPUs. Raising this value increases
static array sizes throughout the codebase; check all `MAX_CPUS`-sized stack
and heap allocations before changing it.

## Documentation Rule

If behavior changes:

1. update code
2. update `README.md`
3. update `README.zh-CN.md`
4. update this file and `DESIGN.zh-CN.md`
5. update `armstat.8`
6. update `EXPORTS.md` and `EXPORTS.zh-CN.md`
7. update `PLOTTING.md` and `PLOTTING.zh-CN.md`
8. update `TESTING.md` and `TESTING.zh-CN.md`

Keep all documents aligned.
