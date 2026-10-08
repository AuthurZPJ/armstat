# armstat

<p align="center">
  <b>English</b> | <a href="README.zh-CN.md">简体中文</a>
</p>

`armstat` monitors ARM64 Linux servers in the style of `turbostat`: CPU
frequency, Busy/Idle, idle states, power and energy, temperature, PMU counters,
and IPC. It reads Linux `sysfs`, `procfs`, `hwmon`, and `perf_event_open()`
interfaces and exports timestamped text, JSON, or CSV. Plotting helpers are
included.

The initial target is **Huawei Kunpeng**. Other ARM64 systems may provide
compatible basic metrics. Each Kunpeng model needs
[target acceptance](docs/REFERENCE.md#arm64-target-acceptance) before deployment;
container tests alone do not establish hardware support.

## Quick Start

Build with a C compiler and `make` on ARM64 Linux, with `/proc` and `/sys`
mounted:

```bash
make
./armstat --probe
./armstat -i 1 -n 5
./armstat -a -i 1 -n 5
./armstat -S -a -f json -O summary.json -i 1 -n 5
sudo make install
```

`--probe` shows which sources are available. PMU/IPC usually requires root or a
permissive `/proc/sys/kernel/perf_event_paranoid`. Optional telemetry depends
on the server's interfaces and sensor mapping.

An installed build provides `armstat`, `armstat-plot-summary`,
`armstat-plot-cpu`, and `man armstat`. Plotting requires Python 3 and matplotlib;
the collector itself has no Python dependency. See the reference for
[cross-compilation](docs/REFERENCE.md#build-and-validation) and
[staged installation](docs/REFERENCE.md#installation).

## Reading the Output

| Mode | Rows per interval |
|---|---|
| Default | One `SUM` row and one row per package |
| `-a` | Aggregate rows plus per-CPU rows; all supported base groups |
| `-S` | One `SUM` row |
| Explicit `-s` | Scopes requested by the selected groups or fields |

The first result follows one complete interval. `-D` measures one interval and
exits; `-n 0` runs until interrupted. The default interval is one second;
the accepted range is `0.000001`–`31536000` seconds. Rate formulas use the
measured window, which can differ from the requested interval.

Multi-row text starts each sample with its timestamp, timezone, and duration.
Summary-only text puts the timestamp at the start of each row. Headers include
units; `CtxSw[/int]`, for example, means a count over the interval. `-q` omits
the banner, headers, timestamps, and interval markers. `-N N` repeats headers
every N samples; layout changes also trigger a new header.

`SUM` combines fields with different scopes:

| Field family | Meaning |
|---|---|
| Frequency, Busy/Idle | Averages across valid selected CPUs |
| LPI states | Each state's average, requiring valid data from every selected CPU |
| PMU | Sum of selected CPUs' interval counts, requiring all CPU groups to be valid |
| System counters | Global interval counts |
| Power, temperature, memory bandwidth | Platform sources with their own sensor scope |

Package rows group selected CPUs by physical package ID. CPU rows use real
Linux CPU IDs and are sorted by ID.

### Important measurements

| Field | Unit | Interpretation |
|---|---|---|
| `Freq`, `Min`, `Max` | MHz | Current frequency sample and configured limits |
| `Idle%`, `Busy%`, `IOWait%` | % | Interval utilization and I/O-wait accounting |
| `LPI-N` | % | State N's cpuidle time increment divided by the interval |
| `LPI-N_usage` | /s | Per-CPU state entry-count increment per second |
| `Power`, `Energy` | mW, J | Package sensor power and estimated interval energy |
| `Temp`, `Temp0` ... `Temp3` | °C | NUMA/die temperature mapping |
| `MemBW` | MiB/s | Rate from a platform memory-read byte counter |
| `CtxSw`, `IRQs`, `SoftIRQs`, PMU | count/interval | Interval counts, not per-second rates |
| `IPC` | instructions/cycle | Instructions divided by cycles |

`Freq` is a point sample, including at `SUM` and package scope. It is not an
interval-average or busy frequency. Display precision does not imply sensor
accuracy. Full [sources and formulas](docs/REFERENCE.md#metric-invariants-and-sources)
are maintained in the reference.

### Busy/Idle and LPI

The default `--busy-source auto` uses `/proc/stat` on every CPU, including
`nohz_full` CPUs. This path includes iowait in `Idle%`; `Busy% = 100 - Idle%`,
so `IOWait%` is an explanatory subset rather than a third utilization bucket.
Short intervals can show `USER_HZ` tick quantization; use about one second when
interpreting utilization.

`--busy-source schedstat` is a diagnostic option with per-CPU procstat
fallback. Task runtime settles at context switches, so ongoing execution can
be missing from an interval. `task-clock` is a compatibility alias for this
path.

Each LPI state reports its own sysfs interval statistic, independent of
`Idle%` and other visible columns. For example, `Idle%=90`, `LPI-0=10`, and
`LPI-1=50` need no correction. Hiding a state does not redistribute its time.
There is no default `IdleGap` column.

cpuidle accounts time on state exit. A long idle period may therefore produce
zero in earlier windows and more than 100% on exit; these values are preserved.
Software accounting does not prove the hardware reached the requested state.
States 0–7 are exposed; deeper states are not folded into another column.
`--probe` maps each `LPI-N` to the kernel's state name.

A missing, disabled, failed, or reset state is unavailable independently.
A recovered cumulative counter needs a new baseline before its next interval.
Unavailable values are `-` in text, `null` in JSON, and empty CSV cells. With
no cpuidle support, LPI columns are hidden and Busy/Idle remains usable.

## Common Commands

```bash
armstat -D                              # One complete interval
armstat -S -a -i 1 -n 10                # Expanded summary
armstat -a -c 0,1,4-7                   # Selected CPUs
armstat -s cpu,freq,idle                # CPU rows and chosen metrics
armstat -s pkg_freq_mhz                 # Package frequency only
armstat -s power,temp                   # Power and temperature
armstat -H temp                         # Hide temperature
armstat -I                             # Cycles, instructions, and IPC
armstat -J                             # Interval energy
```

`--cpu` filters sampling, including PMU resources and CPU-derived aggregates.
Platform sensors and global counters retain their own scope. Default aggregate
rows remain available for the selected CPUs. When CPU rows are explicitly
expanded with a CPU filter, implicit aggregate rows are suppressed; use
`-S --cpu ...` for a filtered summary.

`-s` and `-H` accept groups or exact fields. Repeated `-s` options form a union;
explicit `-p`, `-I`, and `-J` requests remain enabled regardless of their
position relative to `-s`. Unknown tokens, malformed CPU lists, empty CPU
matches, and selections that cannot emit any row are startup errors.

Use `armstat --list` for field IDs, scopes, units, text labels, JSON keys,
column groups, and PMU names. Stable field IDs are the clearest script tokens.
`armstat --help` lists all options.

### PMU

```bash
armstat -p cycles,instructions
armstat -p cache-misses,branches
armstat -p 0x11
```

`-p` selects event counts without automatically adding IPC; `-I` selects
`cycles,instructions` and IPC. An explicit PMU/IPC request without an event
list defaults to those two events. `-a` does not enable PMU/IPC.

Raw events require a `0x` or `0X` prefix and hexadecimal digits within 64 bits.
Unknown or duplicate names and lists over 16 events are rejected. Known events
that cannot be opened remain visible as unavailable values. `mem-read` and
`mem-write` count retired loads and stores, not bytes. Optional events depend
on the CPU. Reduce the CPU or event count if perf groups exceed hardware or
file-descriptor capacity.

## Export and Plot

```bash
armstat -S -a -f json -O summary.json -i 1 -n 60
armstat -a -f csv -O cpus.csv -i 1 -n 60
armstat-plot-summary summary.json --preset freq
armstat-plot-cpu cpus.csv --preset busy --top 8
armstat-plot-cpu cpus.csv --group-by node --y busy
```

`-O` is an alias of `-o`. JSON is an array of interval objects; CSV uses
summary, package, CPU, or mixed rows according to the selected scopes. Both
carry `schema_version=9`, interval number, measured duration, and timestamps.
The bundled plot loaders accept version 9 only.

CSV requires a fixed column layout. A capability change that alters columns
stops capture before the affected interval with a nonzero exit. Restart CSV
or use JSON for changing field sets. Existing output files are opened only
after initialization and baseline collection succeed; later failures retain
the already written prefix.

Plotting preserves timestamps and unavailable-data gaps. Use `--list-fields`
on an input to find usable series. From the source tree, use
`scripts/plot_sum.py` and `scripts/plot_cpu.py` in place of the installed
commands. See [plotting options and behavior](docs/REFERENCE.md#plotting-exports)
and the [export contract](docs/REFERENCE.md#output-contract).

## Platform Limits and Troubleshooting

Power requires exactly one `power_meter/power1_average` source; memory bandwidth
requires exactly one `mem_bytes_read` source. Ambiguous discovery leaves the
metric unavailable and is reported by `--probe`.

The default temperature policy maps `thermal_zoneN/temp` to `TempN` and NUMA/die
N. CPU-row `Temp` uses that CPU's NUMA temperature. If the mapping does not fit
the server, set `ARMSTAT_TEMP_POLICY=none` to disable it. The other accepted
values are `thermal-zone-index` and `disabled`; unknown values fail startup.
This backend exposes neither per-core power nor per-core temperature.

CPU IDs above 1023 are outside the compiled representation; armstat warns and
samples representable online IDs. There are no dedicated core aggregate rows.
Hotplug rebuilds CPU-dependent state and establishes a new baseline before
output resumes.

For missing or unexpected data, start with:

```bash
armstat --probe
armstat --list
armstat -d -i 1 -n 2
```

`--probe` reports source paths, candidate counts, CPU topology, Busy/Idle policy,
and `probe_schema_version: 1`. Its PMU check opens `cycles` on the first selected
CPU only; actual event-by-CPU availability must be tested in a capture.

## Development and Documentation

```bash
make test
make debug-test
make analyze
make target-test    # ARM64 Linux runtime checks
```

- [Reference](docs/REFERENCE.md): architecture, formulas, exports, plotting,
  builds, installation, and target acceptance.
- `man armstat`: installed command-line manual.
- Source-tree `AGENTS.md`: contribution and documentation rules.

The project is licensed under GPL-2.0; see [COPYING](COPYING).
