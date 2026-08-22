# Project Instructions

This file provides context for AI assistants working on this project.

## Project Type

C (Linux kernel style), ARM64 server monitoring tool (`turbostat`-like).

## Build/Test Commands

```bash
make              # Build armstat binary (default: -O2 -Wall -Wextra)
make clean        # Clean build artifacts
make debug        # Rebuild with AddressSanitizer + UBSan (-g -O0)
make test         # Run all tests
make install      # Install to PREFIX (default /usr)
make uninstall    # Remove installed files
```

Cross-compilation: set `CROSS_COMPILE` (e.g. `CROSS_COMPILE=aarch64-linux-gnu-`).
Out-of-tree build: `make O=/path/to/output`.

### Documentation
See README.md for user-facing usage. Architecture, exports, plotting, and
release validation are consolidated in `docs/REFERENCE.md` and
`docs/REFERENCE.zh-CN.md`. The man page is `man/armstat.8`.

## Guidelines

- Follow existing code style and patterns (Linux kernel C style, tabs, snake_case)
- SPDX license headers on source files
- `-Wall -Wextra` enforced; `-D_FORTIFY_SOURCE=2` in default builds
- Write tests for new functionality (add to existing test files in tests/)
- Smoke tests must pass (`make test`)
- Keep changes focused and atomic
- When behavior changes, update all documentation in order:
  Code → README → README.zh-CN → man/armstat.8 → docs/REFERENCE
  → docs/REFERENCE.zh-CN → tests
- Prefer extending the existing README/reference over adding Markdown files

## Important Notes

- Target platform: ARM64 Linux servers (sysfs/procfs/hwmon/perf_event_open)
- MAX_CPUS = 1024, MAX_PMU_EVENTS = 16 (compile-time constants)
- Three-stage pipeline: collect → aggregate → format
- PMU monitoring usually requires root or permissive perf_event_paranoid
- Tests validate code structure and export contracts only; ARM runtime
  behavior requires manual validation on target hardware
