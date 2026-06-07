# armstat Export Contract

This document describes the machine-readable export formats produced by
`armstat`.

Use it when:

- writing downstream parsers
- integrating with pandas, Excel, or plotting scripts
- checking whether an export change is backward compatible

Current export contract version:

- `schema_version = 4`

## Scope

`armstat` exports two machine-readable formats:

- JSON: `armstat -f json`
- CSV: `armstat -f csv`

Both formats are interval-oriented. Each exported record represents one
completed sampling interval.

## Stable metadata

Every exported interval carries these metadata fields:

- `schema_version`
- `interval`
- `timestamp`
- `timestamp_iso`

Meaning:

- `schema_version`
  - Export contract version
- `interval`
  - One-based sample index
- `timestamp`
  - Unix timestamp in seconds
- `timestamp_iso`
  - ISO 8601 wall-clock string for the same sample time

`timestamp` is primarily for programs.
`timestamp_iso` is primarily for people, logs, and manual correlation.

## JSON format

JSON output is a top-level array. Each element is one interval object.

Example:

```json
[
  {
    "schema_version": 4,
    "interval": 1,
    "timestamp": 1774665600,
    "timestamp_iso": "2026-03-28T10:40:00+0800",
    "cpus": [
      {
        "cpu": 0,
        "freq": 2200.00,
        "idle_percent": 99.90,
        "busy_percent": 0.10
      }
    ],
    "summary": {
      "avg_freq": 2200.00,
      "uncore_freq": 1600.00,
      "temp0": 45.00,
      "temp1": 44.00,
      "idle_percent": 99.80,
      "busy_percent": 0.20
    }
  }
]
```

In `schema_version = 4`, summary temperature keys use `temp0`, `temp1`, ...
instead of `temp_vdie0`, `temp_vdie1`, ... and split-idle keys use `lpi0`,
`lpi1`, ... instead of `sum_idle_state0`, `sum_idle_state1`, ...

Likewise, CPU-scoped exports use concise keys such as:

- `freq`, `min`, `max`
- `node`
- `temp`
- `idle_percent`, `iowait_percent`, `busy_percent`
- `lpi0`, `lpi1`, ...

### JSON object shape

Every interval object always contains:

- `schema_version`
- `interval`
- `timestamp`
- `timestamp_iso`

It may also contain:

- `cpus`
- `packages`
- `summary`

Whether `cpus`, `packages`, and `summary` are present depends on output mode:

- default CPU-oriented JSON
  - usually contains `cpus`
  - may also contain `summary` when summary-scope fields are explicitly enabled
- `-S` summary-only JSON
  - contains `summary`
  - omits `cpus`
  - omits `packages`

### JSON CPU objects

Each CPU object always includes:

- `cpu`

It may additionally include:

- CPU-scope fields selected by the current formatter configuration
- `pmu` when PMU output is enabled

Field presence is column-selection dependent. Do not assume every possible CPU
field is always present.

### JSON package objects

When package-scope fields are enabled (e.g. `-s pkg`), each interval may contain
a `packages` array:

```json
"packages": [
  { "package": 0, "package_id": 0, "freq": 2200.00, "idle_percent": 95.0, "busy_percent": 5.0 }
]
```

Each package object always includes `package` (the physical package/socket id
reported by topology). When the package group itself is selected, the field
table also exposes `package_id`; that key is intentionally distinct so JSON
objects never contain duplicate `package` keys.

### JSON summary object

The summary object contains the enabled summary-scope fields. It may also
contain:

- `pmu`

when PMU output is enabled and aggregated summary PMU values exist.

### Null values

JSON uses `null` for unavailable values, for example:

- unavailable temperatures
- unavailable PMU values
- disabled or unsupported split idle states

## CSV format

CSV output is row-oriented. The header is emitted once at the beginning.

Every row starts with:

- `schema_version`
- `interval`
- `timestamp`
- `timestamp_iso`

After that, CSV has one of these layouts:

- summary-only rows
- CPU-only rows
- mixed-scope rows when both summary and CPU sections are enabled

### Summary CSV

Typical command:

```bash
armstat -S -f csv -O summary.csv
```

Header shape:

```text
schema_version,interval,timestamp,timestamp_iso,SUM,...
```

Data rows:

- one row per interval
- `SUM` in the row-identity column
- followed by enabled summary-scope fields
- followed by aggregated PMU columns when PMU is enabled

### CPU CSV

Typical command:

```bash
armstat -f csv -O cpus.csv
```

Header shape:

```text
schema_version,interval,timestamp,timestamp_iso,CPU,...
```

Data rows:

- one row per exported CPU per interval
- `CPU` contains the real CPU ID
- followed by enabled CPU-scope fields
- followed by PMU columns when PMU is enabled

### Mixed-scope CSV

Typical command:

```bash
armstat -f csv -s cpu,power -O mixed.csv
```

Header shape:

```text
schema_version,interval,timestamp,timestamp_iso,Scope,CPU,...
```

Data rows:

- one summary row plus zero or more CPU rows per interval
- `Scope=SUM` rows carry summary-scope fields
- `Scope=CPU` rows carry CPU-scope fields
- `CPU` is empty for `Scope=SUM`
- mixed-scope headers are prefixed to remain machine-readable:
  - `summary.<field>` for summary fields
  - `cpu.<field>` for CPU fields
  - `summary.pmu.<event>` / `cpu.pmu.<event>` for PMU fields
- fields outside the row's scope are emitted as empty CSV cells

This keeps CSV aligned with the text/JSON behavior when summary and CPU
sections are both explicitly enabled.

### CSV quoting

CSV cells are quoted when required. This includes values containing:

- commas
- double quotes
- newlines
- carriage returns

Downstream tooling should parse the file as normal CSV rather than splitting
on commas manually.

## Strings and escaping

JSON string fields are escaped before emission.
CSV fields are quoted when necessary.

This means downstream parsers can safely consume:

- governor names
- PMU event names
- ISO timestamps

without relying on today’s field contents remaining simple forever.

## Summary vs CPU exports

Use summary exports when you want:

- one row/object per interval
- whole-system trends
- plotting `SUM`-scope series

Use CPU exports when you want:

- per-CPU time series
- CPU ranking, filtering, or grouping
- CPU-level plotting

## Compatibility guidance

Treat `schema_version` as the compatibility gate.

Compatible changes within a schema version are expected to be additive, such
as:

- adding new optional fields
- adding new PMU event names
- adding new temperature lines when available
- adding optional platform summary fields such as `uncore_freq`

Changes that remove, rename, or fundamentally reshape fields should use a new
schema version.

## Installed locations

When `armstat` is installed with `make install`, this document is installed
under:

```text
share/doc/armstat/EXPORTS.md
```

See also:

- `PLOTTING.md`
- `TESTING.md`
- `README.md`
