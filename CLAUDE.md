# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`armstat` is an ARM64 server monitoring tool in the style of `turbostat`, focusing on interval-based observation of CPU frequency, busy/idle time, package power/energy, NUMA/package temperature, PMU counters, and topology metadata.

## Build Commands

```bash
make              # Build armstat binary
make clean        # Clean build artifacts
make test         # Run smoke tests (test_column_selection + test_plot_loaders.py)
```

## Architecture

The implementation follows a three-stage pipeline:

```
armstat_cli.c → armstat.c → collector.c → aggregator.c → formatter.c
                        ↓
                  sample_cache.c (memory pools + fast/slow sampling)
                  cpu_inventory.c (CPU identity model - single source of truth)
                  idle_backend.c (busy-source policy: /proc/stat vs /proc/schedstat)
                  power_sensor.c / power_interval.c / membw.c
                  pmu.c (perf-based PMU collection)
                  topology.c / cpufreq.c / cpuidle.c / sysstat.c
```

Key design decisions:
- **CPU Identity Model**: Two identities coexist - `cpu_id` (real Linux CPU ID, external) and `tracked_idx` (dense internal array index). Output rows are sorted by real CPU ID.
- **Three Sampling Layers**: Static/rebuild (topology, inventory) → Slow-changing (~5s refresh with budgeted cursor) → Per-interval fast path (frequency, /proc/stat, power, PMU)
- **Two-Stage Formatting**: `formatter_record.c` builds a stable `interval_record`, then `formatter_text.c`/`formatter_machine.c` serialize to text/JSON/CSV
- **CPU inventory** owns the single source of truth for present/online/tracked CPUs; hotplug rebuilds cascade through the entire stack

## Data Flow Per Interval

1. `sample_cache.c` captures raw cumulative counters (`/proc/stat` idle/iowait, `/proc/schedstat` runtime)
2. `collector.c` orchestrates per-interval collection
3. `aggregator.c` derives interval deltas and percentages (Idle%, Busy%, IOWait%, AvgMHz, power, energy, PMU, IPC)
4. `formatter_record.c` builds `interval_record` from snapshot + stats
5. `formatter_text.c` / `formatter_machine.c` serialize to output

## Important Semantics

- First visible sample is emitted after one full interval, not at startup
- `Idle% + Busy% = 100`; `IOWait%` is independent and not subtracted from `Busy%`
- `LPI-*` columns sum to `Idle%`; deepest visible state is residual-adjusted
- `SUM` aggregates summary-scope fields across tracked CPUs; percentages are averages, not wholes
- When `--cpu` filtering is active, automatic mixed `SUM` section is suppressed
- Default `auto` busy-source prefers `/proc/schedstat` on `nohz_full` CPUs, `/proc/stat` elsewhere

## Metric Sources

| Field | Source | Formula |
|-------|--------|---------|
| Freq | scaling_cur_freq | khz / 1000 |
| Idle% | selected busy-source policy | idle_delta_us / interval_delta_us * 100 |
| Busy% | derived | 100 - Idle% |
| IOWait% | /proc/stat iowait | iowait_delta_us / interval_delta_us * 100 |
| LPI-* | cpuidle/stateN/time | state_delta_us / interval_delta_us * 100 (display-adjusted) |
| Power(mW) | power_meter/power1_average | raw hwmon reading |
| Joules | derived | interval_avg_power_mw * interval_seconds / 1000 |
| MemBW | platform-specific | (counter_now - counter_prev) / interval_seconds |
| IPC | derived | instructions / cycles |

## Hotplug Rebuild Chain

When CPU membership changes: rebuild cpu_inventory → sample_cache → cpuidle → PMU → topology → reset aggregator → treat as fresh baseline.

## Key Files

- `armstat.c`: main loop, module lifecycle
- `armstat_cli.c`: CLI parsing, column selection
- `collector.c`: orchestrates interval collection
- `sample_cache.c`: memory pools, fast/slow refresh with budgeted cursor
- `cpu_inventory.c`: CPU identity and inventory (single source of truth)
- `idle_backend.c`: busy-source policy (/proc/stat vs /proc/schedstat)
- `aggregator.c`: delta/percentage calculations
- `formatter_record.c`: interval_record builder and field table
- `formatter_text.c` / `formatter_machine.c`: output serializers
- `power_sensor.c`: platform sensor discovery

## Documentation Rule

When behavior changes, update in order: code → README.md → README.zh-CN.md → DESIGN.md → DESIGN.zh-CN.md → armstat.8

## Documentation Files

- `README.md`: User-facing overview and usage
- `DESIGN.md`: Implementation details, architecture, data model
- `TESTING.md`: Testing workflow (local build check, smoke tests, target ARM validation)
- `EXPORTS.md`: JSON/CSV export contract
- `PLOTTING.md`: Helper plotting scripts usage
