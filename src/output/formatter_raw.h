/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_FORMATTER_RAW_H
#define ARMSTAT_FORMATTER_RAW_H

struct sys_snapshot;

/* Write cumulative counters and current readings; return -1 on write failure. */
int serialize_raw_snapshot(const struct sys_snapshot *snapshot);

#endif /* ARMSTAT_FORMATTER_RAW_H */
