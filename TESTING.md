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

## 1. Local Build Check

From the `tools/power/armstat` directory:

```bash
make clean
make
```

This confirms:

- the C sources still build cleanly
- the formatter/export path still links correctly
- the helper scripts remain compatible with the current source tree

For memory/undefined-behavior debugging, there is also a sanitizer build:

```bash
make debug
```

That target rebuilds `armstat` with AddressSanitizer and UBSan enabled. It is
intended for local debugging rather than normal release builds.

## 2. Smoke Tests

Run:

```bash
make test
```

This currently executes:

- `tests/test_core_logic`
- `tests/test_column_selection`
- `tests/test_runtime_smoke`
- `tests/test_cli_smoke.sh`
- `tests/test_plot_loaders.py`

The smoke tests intentionally stay small and fast. They verify that:

- idle percentage, busy percentage, iowait, and schedstat clamp calculations are correct

- exact `-s` / `-H` field selection does not accidentally enable or disable an
  entire column group
- `-a` / `all` enables only the base column groups and does not implicitly
  enable PMU or IPC
- hiding `all` clears explicit PMU / IPC state as well
- CLI/runtime combinations such as `-S -a`, `-a -I`, `--probe`, and
  busy-source parsing stay wired correctly
- JSON/CSV serializers still emit the current mixed-scope and summary-only
  layouts end-to-end
- summary JSON exports can still be loaded by `scripts/plot_sum.py`
- summary CSV exports can still be loaded by `scripts/plot_sum.py`
- CPU JSON exports can still be loaded by `scripts/plot_cpu.py`
- CPU CSV exports can still be loaded by `scripts/plot_cpu.py`
- the current machine-readable export contract still matches
  `schema_version = 4`

These tests do **not** validate ARM runtime behavior.

## 3. Target ARM Server Validation

The following commands are the most useful manual checks on a real machine.

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

### Idle / busy semantics

```bash
./armstat -i 1 -n 3 -s cpu,LPI-0,LPI-1,Idle%,IOWait%,Busy%
./armstat -i 1 -n 3 -S --busy-source procstat
./armstat -i 1 -n 3 -S --busy-source schedstat
```

Check:

- `Idle% + Busy%` stays close to `100`
- `IOWait%` is reported separately
- visible `LPI-*` columns remain close to `Idle%`
- `schedstat` behaves sensibly on `nohz_full` CPUs

### PMU / IPC

```bash
./armstat -i 1 -n 2 -I
./armstat -i 1 -n 2 -S -p cycles,instructions
```

Check:

- PMU columns appear when requested
- unsupported PMU data degrades cleanly instead of disappearing silently
- `-I` does not corrupt Busy/Idle values

### NUMA / temperature / power

```bash
./armstat -i 1 -n 2 -s cpu,pkg,core,node,temp
./armstat -i 1 -n 2 -S -s power,temp,energy
./armstat --probe
```

Check:

- CPU rows map to the correct NUMA node
- CPU temperatures match the expected `Temp0..` summary-temperature mapping
- package power appears only at `SUM` scope
- `--probe` reflects the actual platform sensor model, including
  `summary_temp_policy`

### Export validation

```bash
./armstat -S -f json -O summary.json -n 2
./armstat -S -f csv -O summary.csv -n 2
./armstat -f json -O cpus.json -n 2
./armstat -f csv -O cpus.csv -n 2
```

Check:

- exports contain `schema_version`
- CSV contains `timestamp` and `timestamp_iso`
- JSON/CSV can be loaded by the helper scripts

### Plotting smoke checks

```bash
python3 scripts/plot_sum.py summary.json --preset power-temp
python3 scripts/plot_cpu.py cpus.json --preset busy --top 8
```

Check:

- helper scripts accept current exports without manual conversion
- plotting works with the installed `schema_version`

## 4. What Is Covered vs. Not Covered

Currently covered:

- build correctness
- helper script loader compatibility
- machine-readable export contract stability

Not yet covered by automated tests:

- cpuidle behavior on real hardware
- PMU availability and scaling correctness on specific ARM platforms
- hotplug behavior
- nohz_full behavior
- package power / temperature source discovery on real machines

Those still require target-machine validation.
