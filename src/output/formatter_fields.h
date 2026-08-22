/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_fields.h - Private getter declarations
 *
 * Internal contract between:
 *   - columns.c (owns all_fields[] and needs the getter addresses)
 *   - formatter_values.c (defines the getters, reading interval_record)
 *
 * These getters are never called directly; they are only referenced through
 * the field_desc.getter union in all_fields[]. They are therefore not part
 * of the public columns.h surface. Only columns.c and formatter_values.c
 * include this file.
 */

#ifndef ARMSTAT_FORMATTER_FIELDS_H
#define ARMSTAT_FORMATTER_FIELDS_H

#include "columns.h"

/* CPU-scope getters */
int get_cpu_package(const struct interval_record *rec, int row_idx);
int get_cpu_core(const struct interval_record *rec, int row_idx);
int get_cpu_numa_node(const struct interval_record *rec, int row_idx);
double get_cpu_freq_mhz(const struct interval_record *rec, int row_idx);
double get_cpu_min_freq_mhz(const struct interval_record *rec, int row_idx);
double get_cpu_max_freq_mhz(const struct interval_record *rec, int row_idx);
const char *get_cpu_governor(const struct interval_record *rec, int row_idx);
int get_cpu_boost(const struct interval_record *rec, int row_idx);
double get_cpu_busy_percent(const struct interval_record *rec, int row_idx);
double get_cpu_idle_percent(const struct interval_record *rec, int row_idx);
double get_cpu_iowait_percent(const struct interval_record *rec, int row_idx);
double get_cpu_ipc(const struct interval_record *rec, int row_idx);
double get_cpu_temp_c(const struct interval_record *rec, int row_idx);

/* Per-idle-state residency getters (CPU scope) */
double get_cpu_idle_state0(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state1(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state2(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state3(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state4(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state5(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state6(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state7(const struct interval_record *rec, int row_idx);

/* Per-idle-state usage-rate getters (CPU scope) */
double get_cpu_idle_state_usage0(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state_usage1(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state_usage2(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state_usage3(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state_usage4(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state_usage5(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state_usage6(const struct interval_record *rec, int row_idx);
double get_cpu_idle_state_usage7(const struct interval_record *rec, int row_idx);

/* Summary NUMA temp getters (system scope) */
double get_temp_vdie0(const struct interval_record *rec, int row_idx);
double get_temp_vdie1(const struct interval_record *rec, int row_idx);
double get_temp_vdie2(const struct interval_record *rec, int row_idx);
double get_temp_vdie3(const struct interval_record *rec, int row_idx);

/* Summary idle-state getters (system scope) */
double get_summary_idle_state0(const struct interval_record *rec, int row_idx);
double get_summary_idle_state1(const struct interval_record *rec, int row_idx);
double get_summary_idle_state2(const struct interval_record *rec, int row_idx);
double get_summary_idle_state3(const struct interval_record *rec, int row_idx);
double get_summary_idle_state4(const struct interval_record *rec, int row_idx);
double get_summary_idle_state5(const struct interval_record *rec, int row_idx);
double get_summary_idle_state6(const struct interval_record *rec, int row_idx);
double get_summary_idle_state7(const struct interval_record *rec, int row_idx);

/* Other summary getters (system scope) */
double get_summary_avg_mhz(const struct interval_record *rec, int row_idx);
double get_summary_uncore_freq_mhz(const struct interval_record *rec, int row_idx);
double get_summary_busy_percent(const struct interval_record *rec, int row_idx);
double get_summary_idle_percent(const struct interval_record *rec, int row_idx);
double get_summary_iowait_percent(const struct interval_record *rec, int row_idx);
double get_summary_power_mw(const struct interval_record *rec, int row_idx);
double get_summary_energy_joules(const struct interval_record *rec, int row_idx);
double get_summary_mem_bw(const struct interval_record *rec, int row_idx);
double get_summary_ctx_switches(const struct interval_record *rec, int row_idx);
double get_summary_interrupts(const struct interval_record *rec, int row_idx);
double get_summary_soft_interrupts(const struct interval_record *rec, int row_idx);
double get_summary_ipc(const struct interval_record *rec, int row_idx);

/* Package-scope getters */
int get_pkg_package_id(const struct interval_record *rec, int row_idx);
double get_pkg_avg_mhz(const struct interval_record *rec, int row_idx);
double get_pkg_idle_percent(const struct interval_record *rec, int row_idx);
double get_pkg_busy_percent(const struct interval_record *rec, int row_idx);
double get_pkg_iowait_percent(const struct interval_record *rec, int row_idx);
int get_pkg_cpu_count(const struct interval_record *rec, int row_idx);

#endif /* ARMSTAT_FORMATTER_FIELDS_H */
