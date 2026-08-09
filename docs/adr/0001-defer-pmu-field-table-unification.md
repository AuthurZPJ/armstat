# ADR-0001: Defer PMU field-table unification

Date: 2026-08-09

## Status

Deferred — revisit when the field type system is extended or the
mixed-scope CSV layout is redesigned.

## Context

The architecture review (`/improve-codebase-architecture` — export & plotting
focus) identified PMU rendering as the highest-severity duplication in the
formatter layer: 9 PMU-specific functions across `formatter_machine.c` (6) and
`formatter_text.c` (3), all bypassing the `all_fields[]` descriptor table that
governs every other field family. The report rated this candidate "Strong."

Detailed research into the implementation revealed three structural blockers
that make full unification significantly more invasive than the report
estimated:

### Blocker 1: 64-field bitmask cap

`columns.c` uses `uint64_t` override masks (`field_show_mask` /
`field_hide_mask`) for the `-s`/`-H` field-override mechanism. A
`_Static_assert(NUM_FIELDS <= 64)` enforces this. The current field count is
51. Pre-declaring 32 PMU slots (16 CPU-scope + 16 summary-scope, mirroring the
idle-state pattern) would reach 83, blowing the cap.

Resolutions considered:
- Widen masks to `__uint128_t` — touches CLI override plumbing + tests.
- Exempt PMU from per-field overrides — PMU is group-gated via
  `--pmu-events`, not per-event via `-H`, so this is feasible but introduces a
  two-tier override system.
- Runtime-resizable `all_fields[]` — most flexible but most invasive.

### Blocker 2: Type system gap

PMU values are `unsigned long long`. The `field_desc` getter union
(`columns.h`) has `get_double` / `get_llong` / `get_int` / `get_string` but no
`get_ullong`. The existing NaN-placeholder plumbing (used by IPC to render
`null`/`-`/empty when `!pmu_is_active()`) only works for `FIELD_TYPE_DOUBLE`.

Resolutions considered:
- Add `FIELD_TYPE_ULLONG` + `get_ullong` to the union — cleanest, but touches
  `columns.h`, `formatter_record.c`, and all 3 serializer dispatchers.
- Cast to `double` — matches NaN plumbing but loses integer precision for large
  counters.
- Cast to `long long` — uses existing `get_llong` but risks sign issues for
  counters > `LLONG_MAX`.

### Blocker 3: Mixed-scope CSV cross-scope padding

Mixed-scope CSV (`formatter_machine.c:454-632`) emits both summary-scope and
CPU-scope PMU columns in a single header row (`summary.pmu.<name>` +
`cpu.pmu.<name>`), and each row type pads the absent scope's PMU columns with
empty cells. This "both scopes share one row, pad the absent scope" pattern is
unique to PMU and has no analog in the field-table emission path, where each
scope is iterated independently.

Resolutions considered:
- Drop mixed-scope CSV's "both scopes in one row" semantics — a behavior change.
- Keep a residual special-case for mixed-scope CSV even after the rest of PMU
  is table-driven — partially defeats the unification goal.
- Generalize the field table to support cross-scope padding — a much bigger
  refactor affecting every field family.

## Decision

Defer PMU field-table unification until at least one of the structural
blockers is independently addressed. The idle-state pattern in `columns.c`
(`FIELD_SERIES_IDLE_STATE` + per-index visibility + runtime label mutation via
`update_idle_state_visibility()`) is the proven template for the future
implementation.

## Consequences

- The 9 PMU rendering functions remain as parallel code paths in
  `formatter_machine.c` and `formatter_text.c`.
- Adding a new PMU output behavior still requires editing up to 9 functions.
- The `armstat_loader.py` Python alias map (Candidate 2, completed) is the
  single edit point for the Python side — the C side remains the bottleneck.
- Future architecture reviews should NOT re-suggest PMU field-table
  unification without first addressing Blocker 1 (bitmask cap) or Blocker 2
  (type system).

## Revisit triggers

- If `FIELD_TYPE_ULLONG` / `get_ullong` is added to the field type system for
  any other reason, PMU unification becomes mechanically feasible (Blocker 2
  resolved).
- If the override mask scheme is redesigned (e.g. moved to a bitmap struct or
  exempted for series fields), the cap is resolved (Blocker 1).
- If mixed-scope CSV is redesigned to iterate scopes independently, the
  padding problem disappears (Blocker 3).
