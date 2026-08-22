/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_values.c - Typed field accessors for interval records
 *
 * columns.c stores these functions in the field descriptor table. They only
 * read the already materialized interval_record and never collect or derive
 * interval data. Declarations are private to formatter_fields.h.
 */

#include <math.h>

#include "formatter.h"
#include "formatter_fields.h"
#include "pmu.h"
#include "topology.h"
#include "cpu_inventory.h"

/* ============================================================================
 * LOOKUP HELPERS
 * ============================================================================ */

static int get_tracked_cpu_id(int tracked_idx)
{
	return get_cpu_id_by_tracked_idx(tracked_idx);
}

static const struct cpu_row *get_cpu_row(const struct interval_record *rec,
					 int row_idx)
{
	if (!rec || row_idx < 0 || row_idx >= rec->cpu_row_count)
		return NULL;

	return &rec->cpu_rows[row_idx];
}

int get_cpu_row_id(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return -1;

	return get_cpu_id_by_tracked_idx(row->cpu_idx);
}

/* ============================================================================
 * FIELD GETTERS
 * ============================================================================ */

/*
 * Get CPU package ID
 */
int get_cpu_package(const struct interval_record *rec, int row_idx)
{
	int cpu_id;

	(void)rec;
	cpu_id = get_tracked_cpu_id(row_idx);
	return get_package_id(cpu_id);
}

/*
 * Get CPU core ID
 */
int get_cpu_core(const struct interval_record *rec, int row_idx)
{
	int cpu_id;

	(void)rec;
	cpu_id = get_tracked_cpu_id(row_idx);
	return get_core_id(cpu_id);
}

/*
 * Get CPU NUMA node
 */
int get_cpu_numa_node(const struct interval_record *rec, int row_idx)
{
	int cpu_id;

	(void)rec;
	cpu_id = get_tracked_cpu_id(row_idx);
	return get_numa_node(cpu_id);
}

/* --- Frequency getters --- */

double get_cpu_freq_mhz(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row || !row->freq.cur_freq_valid)
		return NAN;
	return row->freq.cur_freq / 1000.0;
}

double get_cpu_min_freq_mhz(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row || !row->freq.min_freq_valid)
		return NAN;
	return row->freq.min_freq / 1000.0;
}

double get_cpu_max_freq_mhz(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row || !row->freq.max_freq_valid)
		return NAN;
	return row->freq.max_freq / 1000.0;
}

const char *get_cpu_governor(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row || !row->freq.governor[0])
		return NULL;
	return row->freq.governor;
}

int get_cpu_boost(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return -1;
	return row->freq.boost < 0 ? -1 : !!row->freq.boost;
}

/* --- Idle/busy getters --- */

double get_cpu_busy_percent(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->busy_percent;
}

double get_cpu_idle_percent(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->idle_percent;
}

double get_cpu_iowait_percent(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->iowait_percent;
}

double get_cpu_ipc(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return NAN;
	if (!pmu_is_active())
		return NAN;
	return row->ipc;
}

/* --- Per-idle-state getters --- */

#define DEFINE_CPU_IDLE_STATE_GETTER(state_idx)				\
double get_cpu_idle_state##state_idx(const struct interval_record *rec,	\
				    int row_idx)			\
{									\
	const struct cpu_row *row = get_cpu_row(rec, row_idx);		\
									\
	if (!row)							\
		return NAN;						\
	return row->idle_state_pct[state_idx];				\
}

DEFINE_CPU_IDLE_STATE_GETTER(0)
DEFINE_CPU_IDLE_STATE_GETTER(1)
DEFINE_CPU_IDLE_STATE_GETTER(2)
DEFINE_CPU_IDLE_STATE_GETTER(3)
DEFINE_CPU_IDLE_STATE_GETTER(4)
DEFINE_CPU_IDLE_STATE_GETTER(5)
DEFINE_CPU_IDLE_STATE_GETTER(6)
DEFINE_CPU_IDLE_STATE_GETTER(7)

/* --- Idle-state usage-rate getters --- */

#define DEFINE_CPU_IDLE_STATE_USAGE_GETTER(state_idx)				\
double get_cpu_idle_state_usage##state_idx(const struct interval_record *rec,	\
				    int row_idx)			\
{									\
	const struct cpu_row *row = get_cpu_row(rec, row_idx);		\
									\
	if (!row)							\
		return NAN;						\
	return row->idle_state_usage[state_idx];				\
}

DEFINE_CPU_IDLE_STATE_USAGE_GETTER(0)
DEFINE_CPU_IDLE_STATE_USAGE_GETTER(1)
DEFINE_CPU_IDLE_STATE_USAGE_GETTER(2)
DEFINE_CPU_IDLE_STATE_USAGE_GETTER(3)
DEFINE_CPU_IDLE_STATE_USAGE_GETTER(4)
DEFINE_CPU_IDLE_STATE_USAGE_GETTER(5)
DEFINE_CPU_IDLE_STATE_USAGE_GETTER(6)
DEFINE_CPU_IDLE_STATE_USAGE_GETTER(7)

/* --- Temperature getters --- */

/*
 * Get per-CPU temperature based on NUMA node
 * CPU belongs to NUMA 0 -> show vdie0, NUMA 1 -> show vdie1
 */
double get_cpu_temp_c(const struct interval_record *rec, int row_idx)
{
	const struct cpu_row *row = get_cpu_row(rec, row_idx);

	if (!row)
		return 0;
	return row->temp_c;
}

/* NUMA temperature getters for SUM level */
static double get_temp_vdie_by_numa(const struct interval_record *rec, int numa)
{
	if (!rec || numa < 0 || numa >= rec->numa_temp_count ||
	    !(rec->numa_temp_valid_mask & (1U << numa)))
		return NAN;
	return rec->numa_temps[numa] / 1000.0;
}

#define DEFINE_NUMA_TEMP_GETTER(numa_idx)				\
double get_temp_vdie##numa_idx(const struct interval_record *rec, int row_idx)	\
{									\
	(void)row_idx;							\
	return get_temp_vdie_by_numa(rec, numa_idx);			\
}

DEFINE_NUMA_TEMP_GETTER(0)
DEFINE_NUMA_TEMP_GETTER(1)
DEFINE_NUMA_TEMP_GETTER(2)
DEFINE_NUMA_TEMP_GETTER(3)

/* --- Package getters --- */

static const struct package_row *get_package_row(
	const struct interval_record *rec,
	int pkg_idx)
{
	if (!rec || pkg_idx < 0 || pkg_idx >= rec->package_count)
		return NULL;

	return &rec->packages[pkg_idx];
}

int get_pkg_package_id(const struct interval_record *rec, int row_idx)
{
	/* cpu parameter is actually package index for package-scope fields */
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->package_id : 0;
}

double get_pkg_avg_mhz(const struct interval_record *rec, int row_idx)
{
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->avg_mhz : 0;
}

double get_pkg_idle_percent(const struct interval_record *rec, int row_idx)
{
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->idle_percent : 0;
}

double get_pkg_busy_percent(const struct interval_record *rec, int row_idx)
{
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->busy_percent : 0;
}

double get_pkg_iowait_percent(const struct interval_record *rec, int row_idx)
{
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->iowait_percent : 0;
}

int get_pkg_cpu_count(const struct interval_record *rec, int row_idx)
{
	const struct package_row *pkg = get_package_row(rec, row_idx);
	return pkg ? pkg->cpu_count : 0;
}

/* --- Summary getters --- */

double get_summary_avg_mhz(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.avg_mhz : NAN;
}

double get_summary_uncore_freq_mhz(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.uncore_freq_mhz : NAN;
}

double get_summary_busy_percent(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.busy_percent : 0;
}

double get_summary_idle_percent(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.idle_percent : 0;
}

double get_summary_iowait_percent(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.iowait_percent : 0;
}

#define DEFINE_SUMMARY_IDLE_STATE_GETTER(state_idx)				\
double get_summary_idle_state##state_idx(const struct interval_record *rec,	\
					 int row_idx)			\
{									\
	(void)row_idx;							\
	if (!rec)							\
		return 0;						\
	return rec->summary_idle_state_pct[state_idx];			\
}

DEFINE_SUMMARY_IDLE_STATE_GETTER(0)
DEFINE_SUMMARY_IDLE_STATE_GETTER(1)
DEFINE_SUMMARY_IDLE_STATE_GETTER(2)
DEFINE_SUMMARY_IDLE_STATE_GETTER(3)
DEFINE_SUMMARY_IDLE_STATE_GETTER(4)
DEFINE_SUMMARY_IDLE_STATE_GETTER(5)
DEFINE_SUMMARY_IDLE_STATE_GETTER(6)
DEFINE_SUMMARY_IDLE_STATE_GETTER(7)

double get_summary_power_mw(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.power_mw : NAN;
}

double get_summary_energy_joules(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.energy_joules : 0;
}

double get_summary_mem_bw(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.mem_bw : NAN;
}

double get_summary_ctx_switches(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.ctx_switches : NAN;
}

double get_summary_interrupts(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.interrupts : NAN;
}

double get_summary_soft_interrupts(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	return rec ? rec->summary.soft_interrupts : NAN;
}

double get_summary_ipc(const struct interval_record *rec, int row_idx)
{
	(void)row_idx;
	if (!rec)
		return NAN;
	if (!pmu_is_active())
		return NAN;
	return rec->summary.ipc;
}
