# armstat Export Contract

<p align="center">
  <a href="README.md">← Back to README</a> |
  <a href="DESIGN.md">Design</a> |
  <a href="PLOTTING.md">Plotting</a> |
  <a href="TESTING.md">Testing</a>
</p>

This document describes the machine-readable export formats produced by
`armstat`.

Use it when:

- writing downstream parsers
- integrating with pandas, Excel, or plotting scripts
- checking whether an export change is backward compatible

Current export contract version:

- `schema_version = 7`

Schema history relevant to consumers:

- version 4 introduced the concise temperature and split-idle key names
- version 5 makes unavailable numeric values explicit instead of serializing
  them as plausible zeroes
- version 6 adds package-only and mixed package CSV rows, including explicit
  `Scope`, `CPU`, and `Package` identity columns, and adds nanosecond sample
  time for subsecond captures
- version 7 adds the measured interval duration, uses RFC 3339 timezone
  offsets, gives summary CSV a normal `Scope` identity column, removes the
  redundant package identity field, and gives strings and booleans explicit
  machine-readable missing-value semantics

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
- `duration_us`
- `timestamp`
- `timestamp_ns`
- `timestamp_iso`

Meaning:

- `schema_version`
  - Export contract version
- `interval`
  - One-based sample index
- `duration_us`
  - Actual elapsed sampling window in monotonic-clock microseconds; use this
    rather than the requested interval when normalizing or auditing a sample
- `timestamp`
  - Unix timestamp in seconds
- `timestamp_ns`
  - Unix timestamp in nanoseconds; use this for subsecond alignment
- `timestamp_iso`
  - RFC 3339 wall-clock string for the same sample time, with nine fractional
    digits and a colon in the UTC offset in schema 7

`timestamp_ns` is the preferred programmatic time axis. `timestamp` remains
for compatibility with consumers that require whole seconds.
`timestamp_iso` is primarily for people, logs, and manual correlation.

## JSON format

JSON output is a top-level array. Each element is one interval object.

Example:

```json
[
  {
    "schema_version": 7,
    "interval": 1,
    "duration_us": 1000123,
    "timestamp": 1774665600,
    "timestamp_ns": 1774665600123456789,
    "timestamp_iso": "2026-03-28T10:40:00.123456789+08:00",
    "cpus": [
      {
        "cpu": 0,
        "freq": 2200.00,
        "governor": "schedutil",
        "boost": true,
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

Since `schema_version = 4`, summary temperature keys use `temp0`, `temp1`, ...
instead of `temp_vdie0`, `temp_vdie1`, ... and split-idle keys use `lpi0`,
`lpi1`, ... instead of `sum_idle_state0`, `sum_idle_state1`, .... Later
versions keep those names.

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
- `duration_us`
- `timestamp`
- `timestamp_ns`
- `timestamp_iso`

It may also contain:

- `cpus`
- `packages`
- `summary`

Whether `cpus`, `packages`, and `summary` are present depends on output mode:

- default CPU-oriented JSON
  - contains `cpus`
  - may also contain `summary` when summary-scope fields are explicitly enabled
  - contains `packages` only when the package group is enabled (e.g. `-s pkg` or `-a`)
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

CPU arrays contain only the tracked, representable CPU set. The current build
represents Linux CPU IDs `0..1023`; when discovery is truncated, armstat writes
a warning to stderr and does not invent rows for unrepresentable IDs.

### JSON package objects

When package-scope fields are enabled (e.g. `-s pkg`), each interval may contain
a `packages` array:

```json
"packages": [
  { "package": 0, "freq": 2200.00, "idle_percent": 95.0, "busy_percent": 5.0 }
]
```

Each package object always includes `package` (the physical package/socket id
reported by topology). The selectable field ID `pkg_id` controls this identity
field; it is not serialized a second time as data.

### JSON summary object

The summary object contains the enabled summary-scope fields. It may also
contain:

- `pmu`

when PMU output is enabled. The value is `null` when no events are configured,
PMU is not active, or the interval is incomplete for any tracked CPU.
PMU event names are object keys and are unique because duplicate names are
rejected during CLI validation.

### Missing values and booleans

Since schema version 5, JSON uses `null` for unavailable numeric values, for
example:

- frequency data after a failed current-frequency read
- Busy/Idle, iowait, and system counters while procstat data is unavailable
- power, energy, memory bandwidth, and sparse temperature sensors when their
  current source is unavailable
- package power or memory bandwidth when discovery finds multiple ambiguous
  candidate sources (armstat does not choose a nondeterministic first match)
- IPC (CPU and summary) when PMU is not active
- unavailable or re-baselining split idle states (`lpi0`-`lpi7`)
- split-idle wakeup fields for missing, disabled, or unusable states
- the `pmu` object when PMU is inactive or any tracked CPU lacks a complete
  interval

A valid source value of zero remains numeric `0`/`0.00`. Consumers must not
coerce `null` to zero when graphing or aggregating intervals. The serializer
also normalizes any non-finite internal floating-point value to `null`, so JSON
never emits non-standard `NaN` or infinity tokens.

In schema version 7, unavailable strings such as `governor` are also `null`
rather than an empty string. `boost` is `true` or `false` when the kernel
exports a value and `null` when boost state is unavailable. Text and CSV keep
their conventional `1`/`0` representation for available boost state.

## CSV format

CSV output is row-oriented. The header is emitted once at the beginning.

Every row starts with:

- `schema_version`
- `interval`
- `duration_us`
- `timestamp`
- `timestamp_ns`
- `timestamp_iso`

After that, CSV has one of these layouts:

- summary-only rows
- package-only rows
- CPU-only rows
- mixed-scope rows when two or more of summary, package, and CPU are enabled

### Summary CSV

Typical command:

```bash
armstat -S -f csv -O summary.csv
```

Header shape:

```text
schema_version,interval,duration_us,timestamp,timestamp_ns,timestamp_iso,Scope,...
```

Data rows:

- one row per interval
- `SUM` in the `Scope` row-identity column
- followed by enabled summary-scope fields
- followed by aggregated PMU columns when PMU is enabled

### CPU CSV

Typical command:

```bash
armstat -f csv -O cpus.csv
```

Header shape:

```text
schema_version,interval,duration_us,timestamp,timestamp_ns,timestamp_iso,CPU,...
```

Data rows:

- one row per exported CPU per interval
- `CPU` contains the real CPU ID
- followed by enabled CPU-scope fields
- followed by PMU columns when PMU is enabled

### Package CSV

Typical command:

```bash
armstat -f csv -s pkg_avg_freq -O packages.csv
```

Header shape:

```text
schema_version,interval,duration_us,timestamp,timestamp_ns,timestamp_iso,Package,...
```

Data rows:

- one row per discovered package per interval
- `Package` contains the physical package/socket ID
- followed by enabled package-scope fields
- exact package field IDs automatically enable this section, so selecting
  `pkg_avg_freq` cannot silently produce a header-only file
- PMU data is not package-scoped and is therefore not appended to these rows

### Mixed-scope CSV

Typical command:

```bash
armstat -a -f csv -O mixed.csv
```

Header shape:

```text
schema_version,interval,duration_us,timestamp,timestamp_ns,timestamp_iso,Scope,CPU,Package,...
```

Data rows:

- enabled scopes are emitted in `SUM`, `PKG`, then `CPU` order per interval
- `Scope=SUM` rows carry summary-scope fields
- `Scope=PKG` rows carry package-scope fields
- `Scope=CPU` rows carry CPU-scope fields
- `CPU` is populated only for `Scope=CPU`
- `Package` is populated only for `Scope=PKG`
- mixed-scope headers are prefixed to remain machine-readable:
  - `summary.<field>` for summary fields
  - `package.<field>` for package fields
  - `cpu.<field>` for CPU fields
  - `summary.pmu.<event>` / `cpu.pmu.<event>` for PMU fields
- fields outside the row's scope are emitted as empty CSV cells
- unavailable numeric or string fields are also emitted as empty CSV cells;
  valid zeroes remain numeric zero; this includes all non-finite internal
  floating-point values

This keeps CSV aligned with text/JSON when any combination of summary,
package, and CPU sections is explicitly enabled. Schema 4/5 mixed CSV used
only `Scope,CPU` and had no `PKG` rows; schema 6 introduced the current mixed
identity layout. Consumers should use `schema_version` to distinguish them.

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

## Types, units, and precision

`armstat --list` is the authoritative discovery view for every exact field ID.
It reports scope, data type, unit, text label, and JSON key from the same field
descriptor table used by all serializers.

Canonical units are:

- frequency: `MHz`
- utilization and idle residency: `%`
- temperature: `degC`
- package power: `mW`
- interval energy: `J`
- memory bandwidth: `MiB/s` (bytes per second divided by 1024 squared)
- idle-state wakeups: `/s`
- context switches and interrupts: `count/interval`
- IPC: `instructions/cycle`

Topology identities, governor names, and CPU/package IDs have no physical
unit. `Boost` is typed as a boolean. Count-per-interval values are rendered
without a fractional suffix; other numeric precision is field-defined and
must not be treated as a statement of sensor accuracy.

## Choosing an export scope

Use summary exports when you want:

- one row/object per interval
- whole-system trends
- plotting `SUM`-scope series

Use CPU exports when you want:

- per-CPU time series
- CPU ranking, filtering, or grouping
- CPU-level plotting

Use package exports when you want:

- socket-level frequency or utilization
- one row per package without the much larger per-CPU row set
- topology-aware comparisons between sockets

## Compatibility guidance

Treat `schema_version` as the compatibility gate.

Consumers that accepted version 4 can usually accept version 5 after ensuring
that JSON `null` and empty CSV cells become missing data rather than zero.
Version 6 JSON adds `timestamp_ns` while retaining the version 5 section
shapes. CSV consumers must additionally accept the `timestamp_ns` metadata
column, optional `Package` identity column, `package.*` fields, and `PKG` rows
in mixed output. Version 7 adds `duration_us`; changes `timestamp_iso` offsets
from `+hhmm` to RFC 3339 `+hh:mm`; renames the summary-only CSV identity header
from `SUM` to `Scope`; serializes package identity only as `package`/`Package`;
uses JSON booleans for `boost`; and uses JSON `null` for unavailable strings.
The bundled summary/CPU plotting loaders accept versions 4 through 7 and
ignore rows outside their requested scope.

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
