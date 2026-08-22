# armstat Plotting Guide

<p align="center">
  <a href="README.md">← Back to README</a> |
  <a href="EXPORTS.md">Export Format</a>
</p>

This document covers the helper plotting scripts shipped with `armstat`.

These scripts are intentionally separate from the main `armstat` command:

- `armstat` is responsible for collecting and exporting interval data
- helper scripts are responsible for turning exported data into time-series
  plots

The current helper scripts are:

- `scripts/plot_sum.py` for `SUM`-scope plots
- `scripts/plot_cpu.py` for CPU-scope plots

Both scripts import shared loaders and field aliases from
`scripts/armstat_loader.py`, and matplotlib helpers from
`scripts/plot_utils.py`. Both scripts work with `armstat` JSON exports and
current CSV exports.

The exact machine-readable export contract is documented separately in
`EXPORTS.md`.

When `armstat` is installed with `make install`, the helper scripts and
plotting docs are also installed under `share/doc/armstat/`.

For JSON inputs, these scripts load the full export into memory. For CSV inputs with `--sample-range`, only the requested samples are loaded. They are best
suited to moderate-size traces, interactive analysis, and filtered CPU exports.
For very long runs or very large CPU dumps, prefer:

- `SUM` plots first
- `--cpu-filter`, `--top`, or `--group-by` for CPU plots
- exporting shorter windows when possible

## Dependencies

These scripts require matplotlib. Install it with:

```bash
python3 -m pip install matplotlib
```

## Input Exports

### Summary export

```bash
armstat -S -f json -O summary.json
armstat -S -f csv -O summary.csv
```

### CPU export

```bash
armstat -f json -O cpus.json
armstat -f csv -O cpus.csv
```

Current CSV exports include:

- `schema_version`
- `interval`
- `duration_us`
- `timestamp`
- `timestamp_ns`
- `timestamp_iso`

So plotting can use real time directly.

The timestamp fields represent the same sample time in different formats:

- `timestamp`: whole Unix seconds, retained for compatibility
- `timestamp_ns`: Unix nanoseconds, preferred for plotting and subsecond input
- `timestamp_iso`: RFC 3339 wall-clock string with fractional seconds

`duration_us` is the actual measured sampling window. It is retained as
metadata for downstream analysis but is not offered as a default plotted
metric.

The plotting scripts prefer `timestamp_ns` and fall back to `timestamp` for
schema 4/5 input. `timestamp_iso` keeps exports easy to inspect and correlate
manually.

Current machine-readable exports use `schema_version = 7`. The bundled loaders
accept versions 4 through 7 so existing traces remain usable. In mixed CSV,
they skip `PKG` rows and `package.*` columns when loading SUM or CPU plots.
See `EXPORTS.md` for the exact JSON/CSV structure.

Unavailable values introduced in schema 5 (`null` in JSON and empty CSV cells)
are loaded as `NaN`. Plots therefore show a gap instead of a false zero;
smoothing and group aggregation ignore missing points when valid peers are
available. This also covers package-power or memory-bandwidth fields disabled
because source discovery was ambiguous.

If armstat reports CPU-inventory truncation on stderr, CPU plots contain only
the representable exported IDs (`0..1023`). The loaders do not synthesize lines
for higher IDs; reduce the source system CPU set or rebuild with a larger
`MAX_CPUS` before treating such a plot as whole-machine coverage.

## Summary Plots

Use `plot_sum.py` for `SUM`-scope time series.

### Common commands

```bash
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset freq
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset power
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset temp
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset power-temp
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset idle-lpi
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset sysstat

python3 tools/power/armstat/scripts/plot_sum.py summary.json --y busy
python3 tools/power/armstat/scripts/plot_sum.py summary.json --y power --y2 temp0
python3 tools/power/armstat/scripts/plot_sum.py summary.json --list-fields
```

### Summary presets

- `freq`
- `power`
- `temp`
- `power-temp`
- `idle-lpi`
- `sysstat`

Notes:

- `--preset freq` plots `avg_freq`, and also `uncore_freq` when the export
  contains it
- `--y freq` resolves to `avg_freq` for a summary export and to `freq` for a
  CPU export
- `--preset temp` plots all available `temp*` lines
- `--preset power-temp` plots `power` on the left axis and all available
  `temp*` lines on the right axis
- `--preset idle-lpi` is useful for `Busy% / Idle% / LPI-*` trends
- `--preset sysstat` is useful for `CtxSw / IRQs / SoftIRQs / MemBW`

### Useful options

- `--y FIELD`
- `--y2 FIELD`
- `--sample-range START:END`
- `--smooth N`
- `--output-dir DIR`
- `--format png|svg|pdf`
- `--title TEXT`

Examples:

```bash
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset idle-lpi --sample-range 10:200 --smooth 5
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset sysstat --output-dir plots --format svg
python3 tools/power/armstat/scripts/plot_sum.py summary.csv --y freq
```

## CPU Plots

Use `plot_cpu.py` for per-CPU or grouped CPU time series.

### Common commands

```bash
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --preset freq --cpu-filter 0,2,4
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --preset temp --cpu-filter 0,2,4
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --preset busy --top 8
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --preset busy --top 8 --rank-by max

python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --y busy --cpu-filter 0,2,4
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --y temp --cpu-filter 0-31 --top 4
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --y freq --y2 temp --cpu-filter 0
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --y lpi1 --cpu-filter 0,2,4

python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --group-by node --y busy
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --group-by core --y temp --top 8
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --group-by node --aggregate max --y temp

python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --y busy --sample-range 10:200 --smooth 5 --cpu-filter 0-31 --top 8
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --preset freq --output-dir plots --format svg --top 4

python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --list-fields
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --list-cpus
```

### CPU presets

- `freq`
- `temp`
- `idle`
- `busy`
- `iowait`
- `ipc`

### Field aliases

The script accepts human-friendly aliases in addition to raw export field names.

Examples:

- `freq` -> `freq` for CPU data (`avg_freq` for summary data)
- `temp` -> `temp`
- `busy` -> `busy_percent`
- `idle` -> `idle_percent`
- `iowait` -> `iowait_percent`
- `lpi1` -> `lpi1`
- `cycles` -> `pmu.cycles`
- `instructions` -> `pmu.instructions`

PMU event paths are unambiguous because armstat rejects duplicate event names
before sampling starts.

### CPU selection

- `--cpu-filter 0,1,4-7`
  - explicit CPU list, using syntax similar to `armstat -c`
- `--top N`
  - selects the top N entities ranked by the primary field
- `--rank-by avg|max|last`
  - controls how `--top` ranks:
    - `avg`: time-average
    - `max`: peak value
    - `last`: last visible sample

On large exports, the script refuses to draw every CPU by default if the result
would likely be unreadable. In practice, use one of:

- `--cpu-filter`
- `--top`
- `--group-by`

### Grouped plots

`--group-by` aggregates multiple CPUs into one line.

Supported grouping:

- `--group-by node`
- `--group-by core`

Supported within-group aggregation:

- `--aggregate avg`
- `--aggregate max`
- `--aggregate min`

Semantics:

- grouping happens first
- if `--top` is also used, ranking is applied to groups rather than individual
  CPUs
- grouped plots use the selected field aggregated across the CPUs inside each
  group for every sample

### Other useful options

- `--y FIELD`
- `--y2 FIELD`
- `--sample-range START:END`
- `--smooth N`
- `--output-dir DIR`
- `--format png|svg|pdf`
- `--title TEXT`
- `-o, --output PATH`

### Notes

- CPU plots currently focus on fields that `armstat` exports per CPU
- per-CPU power is not currently exported
- grouped plots are often more readable than raw per-CPU plots on large
  systems

### CPU CSV examples

```bash
python3 tools/power/armstat/scripts/plot_cpu.py cpus.csv --cpu-filter 0,2,4 --y freq
python3 tools/power/armstat/scripts/plot_cpu.py cpus.csv --preset busy --top 8
```
