# CONTEXT.md — armstat

Shared vocabulary for armstat. The `/domain-modeling` skill keeps this glossary
current; add a term when a decision resolves one.

## Glossary

- **CPU identity** — the concept and module (`cpu_inventory.c`) that owns CPU
  membership and identity: which CPUs are present, online, and tracked, and how
  the two indexing spaces relate. Per CLAUDE.md it is the single source of truth
  for present/online/tracked CPUs; hotplug rebuilds cascade through it.

- **cpu_id** — the real Linux CPU ID (external namespace, e.g. `0, 4, 8, 12`).
  After the identity refactor it survives only inside CPU identity, at the
  sysfs/procfs/syscall boundary (where the kernel namespaces data by real id,
  e.g. sysfs paths, `/proc/stat` scratch buffers, `perf_event_open`), and as the
  display identifier in output rows. It never drives the internal per-CPU array
  layout, which is indexed exclusively by `tracked_idx`.

- **tracked_idx** — the dense internal array index (`0 .. tracked_count-1`) into
  the tracked CPU set. All per-CPU arrays index by this.

- **present** — a CPU that exists in `/sys/devices/system/cpu`.

- **online** — a CPU that is currently online.

- **tracked** — the online CPUs that armstat actually samples (online count
  limited by `MAX_CPUS`). The set everything iterates over.

- **cpu_desc** — the unified per-CPU descriptor: identity (`cpu_id`, present,
  online) plus topology attributes (`package_id`, `core_id`, `numa_node`,
  `cpu_id_in_core`, ...). What the tracked-CPU iteration hands back.

- **tracked CPU view** — the iteration interface (`for_each_tracked_cpu`) that
  yields a `cpu_desc` per tracked CPU, hiding the `tracked_idx ↔ cpu_id`
  translation from consumers.

- **column visibility** — the concept and module (`columns.c`) that owns the
  `show_*` group-visibility flags, the idle-state and summary-temp series
  visibility + override bitmasks, and the `enable_*()` / `reset_columns()` /
  `clear_columns()` API. Written by CLI parsing; read by sample_cache for
  demand-driven sampling and by the serializers/section policy for output
  decisions. Single owner — no other module defines or mutates the visibility
  state directly.

- **field registry** — the `all_fields[]` descriptor table and its query API
  (`get_field_desc`, `get_enabled_fields`, `any_fields_enabled`,
  `field_is_effectively_enabled`), owned by `columns.c`. Ties each field id
  to its group, scope, series, enabled flag, and value getter. The value
  getters themselves live in `formatter_record.c` (declared in the private
  `formatter_fields.h` sub-header); the registry references them by address.

- **snapshot accessors** — the `sys_snapshot_get_*()` functions declared in
  `collector.h` and implemented in `collector.c`. The seam between the
  collector (producer) and the aggregator/formatter/main-loop (consumers).
  Multi-consumer fields (`effective_cpu_count`, `cpu_truncated`,
  `interval_delta_us`, `freqs`, `counters`) are read through these getters;
  single-consumer fields remain directly accessed. A future step can make
  `struct sys_snapshot` fully opaque by adding the remaining getters and
  moving the struct definition into `collector.c`.

- **sysfs I/O primitives** — the shared read functions in `sysfs_util.c` /
  `sysfs_util.h` (`sysfs_read_int_checked`, `sysfs_read_ull_checked`,
  `sysfs_read_str`, `sysfs_path_exists`, `fd_read_ull_checked`). The seam
  between all sysfs/procfs/fd readers and the modules that consume them
  (`topology.c`, `power_sensor.c`, `cpufreq.c`, `cpuidle.c`). Consolidates
  previously duplicated patterns with a single checked convention (0 on
  success, -1 on failure, value via out-param).
