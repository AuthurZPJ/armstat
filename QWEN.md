# QWEN.md — armstat Project Context

<p align="center">
  <a href="README.md">← Back to README</a> |
  <a href="DESIGN.md">Design</a> |
  <a href="TESTING.md">Testing</a> |
  <a href="CLAUDE.md">AI Guidelines</a>
</p>

## Project Overview

`armstat` is an ARM64 server monitoring tool in the style of `turbostat`. It provides interval-based observation of CPU frequency, busy/idle time, package power/energy, NUMA temperature, PMU counters (IPC), memory bandwidth, and topology metadata. It targets ARM servers where telemetry comes from `sysfs`, `/proc/stat`, `hwmon`, `thermal_zone`, and `perf_event_open()`.

**Language:** C (with Python helper scripts for plotting)
**License:** GPL-2.0
**Target platform:** ARM64 Linux servers

## Key Differentiator

Produces machine-readable JSON and CSV exports with per-sample timestamps and a stable `schema_version` (currently 4). Helper plotting scripts (`scripts/plot_sum.py`, `scripts/plot_cpu.py`) turn exports into time-series charts.

## Architecture

Three-stage pipeline: **collect → aggregate → format**, with a clear separation between raw data capture, interval delta computation, and output serialization.

```
armstat_cli.c (CLI parsing, column selection)
  → armstat.c (main loop, module lifecycle, signals, --probe)
    → collector.c (orchestrates one interval of collection)
      → cpu_inventory.c (single source of truth for present/online/tracked CPUs)
      → sample_cache.c (memory pools, fast/slow sampling layers)
      → idle_backend.c (busy-source policy: /proc/stat vs /proc/schedstat)
      → cpufreq.c / cpuidle.c / power_sensor.c / power_interval.c / pmu.c / membw.c / sysstat.c / topology.c
    → aggregator.c (interval deltas, percentages — no I/O)
    → formatter.c (output facade)
      → formatter_record.c (builds stable interval_record)
      → formatter_text.c / formatter_machine.c (serializers)
```

### Three Sampling Layers

1. **Static/rebuild** — CPU inventory, topology, sensor discovery, cpuidle state names, PMU event metadata
2. **Slow-changing (~5s)** — CPU min/max freq, governor, boost, sensor flags, cpuidle disable state. Uses a rolling cursor + budget to avoid periodic spikes.
3. **Per-interval fast path** — current frequency, `/proc/stat` deltas, package power, NUMA temperatures, PMU counters, cpuidle `stateN/time`

### CPU Identity Model

Two identities coexist: `cpu_id` (real Linux CPU ID, used externally) and `tracked_idx` (dense internal array index). `--cpu` filtering is a sampling filter applied before runtime. Invalid tokens, reversed ranges, and no-match filters are startup errors.

### Hotplug Rebuild Chain

CPU membership change → rebuild cpu_inventory → sample_cache → cpuidle → PMU → topology → reset aggregator → treat as fresh baseline.

### Key Design Constants

- `MAX_CPUS = 1024` (compile-time, in `collector.h`)
- `MAX_PMU_EVENTS = 16`
- `PROC_LINE_MAX = 512`
- cpuidle fd cache capped at 32

## Build Commands

```bash
make              # Build armstat binary (default: -O2 -Wall -Wextra)
make clean        # Clean build artifacts
make debug        # Rebuild with AddressSanitizer + UBSan (-g -O0)
make test         # Run all smoke tests
make install      # Install to PREFIX (default /usr)
```

Cross-compilation: set `CROSS_COMPILE` (e.g., `CROSS_COMPILE=aarch64-linux-gnu-`).
Out-of-tree build: `make O=/path/to/output`.

## Test Commands

```bash
make test
```

Runs these tests in order:
1. `tests/test_core_logic` — idle/busy/iowait/schedstat clamp calculations
2. `tests/test_column_selection` — `-s`/`-H` field selection, `-a`/`all` behavior
3. `tests/test_runtime_smoke` — CLI/runtime combinations (`-S -a`, `-a -I`, `--probe`, busy-source parsing, JSON/CSV serialization, schema_version)
4. `tests/test_cli_smoke.sh` — shell-based CLI smoke checks
5. `tests/test_plot_loaders.py` — verifies plotting scripts can load current exports

**Note:** Tests validate code structure and export contracts only. ARM runtime behavior (cpuidle, PMU, hotplug, nohz_full, power/temp sensors) requires manual validation on target hardware. See `TESTING.md` for target-machine validation commands.

## Usage Examples

```bash
armstat                        # Default: per-package + CPU rows, 1s interval
armstat -i 5                   # 5-second intervals
armstat -n 10                  # 10 iterations then exit
armstat -D                     # One-shot (single iteration)
armstat -S                     # Summary-only mode (one SUM row)
armstat -S -a                  # Summary + all base column groups
armstat -c 0,1,4-7            # CPU filter (real Linux CPU IDs)
armstat -s power,temp,freq    # Column group selection
armstat -H temp                # Hide specific columns
armstat -f json -O data.json   # JSON export
armstat -f csv -O data.csv     # CSV export
armstat -I                     # Enable PMU (cycles, instructions, IPC)
armstat -p cache-misses,branches  # Custom PMU events
armstat --busy-source auto     # Default: /proc/stat + schedstat for nohz_full
armstat --probe                # One-shot platform capability dump
armstat -l                     # List built-in column groups and PMU events
```

## Important Semantics

- **First output is after one full interval**, not at startup.
- `Idle% + Busy% = 100`; `IOWait%` is independent, not subtracted from `Busy%` (off by default, enable with `-s IOWait%` or `-s idle`).
- `LPI-*` columns are display-adjusted: deepest visible usable state absorbs residual so `sum(LPI-*) ≈ Idle%`. Up to 8 visible columns.
- `SUM` row aggregates summary-scope fields; percentages are tracked-CPU averages.
- `--cpu` suppresses the automatic mixed `SUM` section.
- `--cpu` also suppresses per-package aggregation rows.
- Default busy-source (`auto`): `/proc/stat` for ordinary CPUs, `/proc/schedstat` for `nohz_full` CPUs.

## Development Conventions

### Code Style
- Linux kernel C style (tabs for indentation, `snake_case` functions, `UPPER_CASE` constants)
- SPDX license headers on source files
- `-Wall -Wextra` enforced; `-D_FORTIFY_SOURCE=2` in default builds

### Documentation Rule
When behavior changes, update **all five** in order:
1. Code
2. `README.md`
3. `README.zh-CN.md`
4. `DESIGN.md` / `DESIGN.zh-CN.md`
5. `armstat.8` (man page)

### Testing
- Write tests for new functionality (add to existing test files in `tests/`)
- Smoke tests must pass (`make test`)
- Keep tests small and fast; they run on any dev machine, not just ARM

### Documentation Files
| File | Purpose |
|------|---------|
| `README.md` / `README.zh-CN.md` | User-facing overview and usage |
| `DESIGN.md` / `DESIGN.zh-CN.md` | Implementation details, architecture, data model |
| `TESTING.md` / `TESTING.zh-CN.md` | Testing workflow and validation guidance |
| `EXPORTS.md` / `EXPORTS.zh-CN.md` | JSON/CSV export contract (schema_version) |
| `PLOTTING.md` / `PLOTTING.zh-CN.md` | Helper plotting scripts usage |
| `armstat.8` | Man page |

### Plotting Scripts
- `scripts/plot_sum.py` — plots from summary JSON/CSV exports
- `scripts/plot_cpu.py` — plots from per-CPU JSON/CSV exports
- Both require `matplotlib` and are installed alongside documentation

## Metric Sources Quick Reference

| Field | Source | Formula |
|-------|--------|---------|
| Freq (MHz) | `scaling_cur_freq` | khz / 1000 |
| AvgFreq (MHz) | derived | (prev + cur) / 2, averaged across tracked CPUs |
| Idle% | busy-source policy | idle_delta_us / interval_delta_us × 100 (procstat); 100 - Busy% (schedstat) |
| Busy% | derived | 100 - Idle% (procstat); sched_runtime_delta_ns / wall_clock_delta_ns × 100 (schedstat) |
| IOWait% | `/proc/stat` iowait | iowait_delta_us / interval_delta_us × 100 |
| LPI-* | `cpuidle/stateN/time` | state_delta_us / interval_delta_us × 100 (display-adjusted) |
| Power (mW) | `power_meter/power1_average` | raw hwmon reading |
| Energy (J) | derived | interval_avg_power_mw × interval_s / 1000 |
| MemBW | platform-specific counter | (counter_now - counter_prev) / interval_s |
| IPC | derived | instructions / cycles |
| CtxSw | `/proc/stat` ctxt | ctxt_now - ctxt_prev |
| IRQs | `/proc/stat` intr | intr_now - intr_prev |
| SoftIRQs | `/proc/stat` softirq | softirq_now - softirq_prev |

## File Descriptor Budget

| Subsystem | Open FDs | Cap |
|-----------|----------|-----|
| cpuidle | 1 per CPU × per state | ≤ 32 |
| sysstat | 2 (`/proc/stat`, `/proc/schedstat`) | 2 |
| cpufreq | 1 per CPU (`scaling_cur_freq`) | none |
| PMU | N events × tracked CPUs (1 group/CPU) | none |

On large systems (256+ CPUs with PMU), total can exceed 500 fds. Use `--cpu` to reduce tracked CPU count or raise `ulimit -n`.

## Platform-Specific Notes

- **Power**: Looks for `hwmon` device with `name=power_meter`, reads `power1_average`. Degrades silently if missing.
- **Temperature**: Default `thermal-zone-index` policy maps `thermal_zoneN/temp` → `TempN` → NUMA/Vdie N. This is a platform convention, not a kernel guarantee. Override with `ARMSTAT_TEMP_POLICY=none`.
- **Uncore/DevFreq**: Scans `/sys/class/devfreq/` for uncore/interconnect/fabric device. Falls back silently.
- **PMU**: Requires root or permissive `perf_event_paranoid`. Unknown event names are hard errors; known events that can't be opened degrade to unavailable values.
