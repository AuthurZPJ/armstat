# Project Instructions

`armstat` is a C monitor for ARM64 Linux servers, in the style of `turbostat`.
It reads sysfs/procfs/hwmon/perf interfaces through a collect → aggregate →
format pipeline. PMU usually needs root or permissive `perf_event_paranoid`.
Compile-time limits: `MAX_CPUS = 1024`, `MAX_PMU_EVENTS = 16`.

## Build and Test

| Command | Purpose |
|---|---|
| `make`, `make clean` | Build with `-O2 -Wall -Wextra`; clean artifacts |
| `make debug`, `make debug-test` | ASan/UBSan build; sanitizer tests |
| `make test` | All regression and smoke tests |
| `make analyze` | GCC/Clang path-sensitive analysis |
| `make target-test` | ARM64 Linux runtime acceptance |
| `make install`, `make uninstall` | Install/remove under `PREFIX` (default `/usr`) |

Use `CROSS_COMPILE=aarch64-linux-gnu-` for cross builds and `O=/path/to/output`
for out-of-tree builds. Tests must pass; host structural/export checks do not
prove ARM telemetry behavior. Validate runtime metrics on the target hardware.

## Code and Documentation

- Follow existing Linux kernel C style: tabs, snake_case, SPDX source headers.
- Preserve `-Wall -Wextra` and default `-D_FORTIFY_SOURCE=2` checks.
- Keep changes focused and atomic; add new functionality tests to `tests/`.
- For behavior changes, update in order: code → README.md → README.zh-CN.md
  → man/armstat.8 → docs/REFERENCE.md → docs/REFERENCE.zh-CN.md → tests.
- Extend existing documents before adding Markdown files. README covers use;
  the reference pair covers architecture, formulas, exports, plotting, and
  release validation; `man/armstat.8` is the installed CLI manual.
