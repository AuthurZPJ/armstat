# CLAUDE.md

<p align="center">
  <a href="README.md">← Back to README</a> |
  <a href="DESIGN.md">Design</a> |
  <a href="TESTING.md">Testing</a> |
  <a href="QWEN.md">Project Context</a>
</p>

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
- `Idle% + Busy% = 100`; `IOWait%` is independent and not subtracted from `Busy%` (off by default)
- `LPI-*` columns sum to `Idle%`; deepest visible state is residual-adjusted
- `SUM` aggregates summary-scope fields across tracked CPUs; percentages are averages, not wholes
- When `--cpu` filtering is active, automatic mixed `SUM` section is suppressed
- When `--cpu` filtering is active, per-package aggregation rows are also suppressed
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

---

# Behavioral Guidelines (Karpathy Principles)

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. Think Before Coding
**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First
**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes
**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution
**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.
