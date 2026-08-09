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
