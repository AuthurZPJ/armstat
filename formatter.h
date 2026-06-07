/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter.h - Output formatting layer
 *
 * RESPONSIBLE FOR:
 *   - Text/JSON/CSV output formatting
 *   - Header printing
 *   - Row formatting
 *
 * NOT RESPONSIBLE FOR:
 *   - Reading sysfs/proc
 *   - Calculating statistics
 */

#ifndef ARMSTAT_FORMATTER_H
#define ARMSTAT_FORMATTER_H

#include <time.h>

#include "collector.h"
#include "aggregator.h"

/* Output format types */
#define FORMAT_TEXT  0
#define FORMAT_JSON  1
#define FORMAT_CSV   2

/*
 * Field scope - defines where field data comes from
 */
enum field_scope {
	FIELD_SCOPE_SYSTEM,    /* System-wide: avg_mhz, busy_percent, power, etc. */
	FIELD_SCOPE_PACKAGE,   /* Per-package: avg_mhz, idle, busy, iowait */
	FIELD_SCOPE_CPU,       /* Per-CPU: freq, idle, power, temp */
};

/*
 * Logical column groups exposed through -s/-H.
 *
 * Keep group membership with the field descriptors so CLI selection does not
 * have to re-derive relationships such as "all LPI fields belong to idle" or
 * "all TempN fields belong to temp".
 */
enum field_group_mask {
	FIELD_GROUP_NONE    = 0,
	FIELD_GROUP_PACKAGE = 1U << 0,
	FIELD_GROUP_CORE    = 1U << 1,
	FIELD_GROUP_NUMA    = 1U << 2,
	FIELD_GROUP_FREQ    = 1U << 3,
	FIELD_GROUP_IDLE    = 1U << 4,
	FIELD_GROUP_POWER   = 1U << 5,
	FIELD_GROUP_TEMP    = 1U << 6,
	FIELD_GROUP_SYSSTAT = 1U << 7,
	FIELD_GROUP_MEMBW   = 1U << 8,
	FIELD_GROUP_IPC     = 1U << 9,
	FIELD_GROUP_ENERGY  = 1U << 10,
};

/*
 * Optional field series metadata.
 *
 * Most fields are standalone, but some belong to indexed families such as
 * split idle-state residency or summary TempN values. Keep that identity in
 * field metadata so CLI selection does not have to infer it from field ids.
 */
enum field_series {
	FIELD_SERIES_NONE = 0,
	FIELD_SERIES_IDLE_STATE,
	FIELD_SERIES_SUMMARY_TEMP,
};

/*
 * Field type - defines the data type of a field
 */
enum field_type {
	FIELD_TYPE_INT,
	FIELD_TYPE_LLONG,
	FIELD_TYPE_DOUBLE,
	FIELD_TYPE_STRING,
};

/*
 * Field getter functions - used for data-driven serialization
 */
struct interval_record;
struct cpu_row;
struct summary_data;

/* Getter function types */
typedef double (*getter_double)(const struct interval_record *rec, int cpu);
typedef long long (*getter_llong)(const struct interval_record *rec, int cpu);
typedef int (*getter_int)(const struct interval_record *rec, int cpu);
typedef const char *(*getter_string)(const struct interval_record *rec, int cpu);

/*
 * Field descriptor - metadata for a single field
 * This enables centralized field management instead of scattered printf logic
 */
struct field_desc {
	const char *id;           /* JSON key: "avg_mhz", "cpu0_cycles" */
	const char *label;        /* Column header: "AvgFreq", "cpu0_cycles" */
	const char *json_label;   /* JSON label (can differ from id) */
	enum field_scope scope;   /* Data scope */
	enum field_type type;      /* Data type */
	unsigned int group_mask;  /* -s/-H logical column groups */
	enum field_series series; /* Indexed field family, if any */
	int series_index;         /* Index within the field family */
	int *enabled_ptr;         /* Pointer to show_* variable */

	/* Getter functions - based on type */
	union {
		getter_double get_double;
		getter_llong get_llong;
		getter_int get_int;
		getter_string get_string;
	} getter;
};

/*
 * Column visibility — read by sample_cache for demand-driven sampling,
 * written by armstat.c CLI parsing via the enable_*() functions below.
 *
 * Do NOT modify these directly from other modules.  Use the enable_*()
 * setters or the is_*_enabled() getters.
 */
extern int show_cpu;
extern int show_freq;
extern int show_idle;
extern int show_iowait;
extern int show_power;
extern int show_temp;
extern int show_pmu;
extern int show_sysstat;
extern int show_membw;
extern int show_package;
extern int show_core;
extern int show_numa;
extern int show_ipc;
extern int show_energy;

/* Convenience getters for demand-driven sampling decisions. */
static inline int is_freq_enabled(void)    { return show_freq; }
static inline int is_power_enabled(void)   { return show_power; }
static inline int is_temp_enabled(void)    { return show_temp; }
static inline int is_energy_enabled(void)  { return show_energy; }
static inline int is_pmu_enabled(void)     { return show_pmu; }
static inline int is_ipc_enabled(void)     { return show_ipc; }

struct field_desc *get_field_desc(const char *field_id);
struct field_desc *get_all_fields(void);
int get_field_count(void);
int any_fields_enabled(enum field_scope scope);
void get_enabled_fields(enum field_scope scope, struct field_desc **fields, int *count);
void clear_field_overrides(void);
void set_field_override_by_index(int field_index, int enable, int whitelist);

/*
 * CPU row data - per-CPU intermediate model
 * OPTIMIZATION: Only stores cpu_idx, static fields (package, core, numa_node, governor)
 * are looked up at output time from topology/cpufreq caches.
 */
struct cpu_row {
	int cpu_idx;  /* Index into raw->freqs[], raw->powers[], stats->per_cpu_* arrays */
};

/*
 * Summary data - system-wide aggregated statistics
 */
struct summary_data {
	/* Frequency */
	double avg_mhz;
	double uncore_freq_mhz;
	double busy_percent;
	double idle_percent;
	double iowait_percent;

	/* Power/Energy */
	long long power_mw;
	double energy_joules;

	/* Memory bandwidth */
	unsigned long long mem_bw;

	/* Sysstat */
	unsigned long ctx_switches;
	unsigned long interrupts;
	unsigned long soft_interrupts;

	/* PMU events (aggregated in summary mode) */
	unsigned long long pmu[MAX_PMU_EVENTS];
	int pmu_count;

	/* IPC */
	double ipc;
};

/*
 * Interval record - unified intermediate model
 * Built from sys_snapshot + interval_stats, consumed by serializers
 *
 * OPTIMIZATION: cpu_rows only stores cpu_idx (index into arrays).
 * Static fields (package, core, numa_node, governor) are looked up at output time.
 * Dynamic fields (freq, power, temp, busy/idle) accessed via raw/stats pointers.
 */
struct interval_record {
	int interval;
	time_t timestamp;

	/* CPU info */
	int cpu_count;           /* Tracked CPU count */
	int cpu_count_filtered;  /* Tracked count after --cpu sampling filter */
	int cpu_truncated;       /* Truncation warning flag */

	/* PMU event count - global, used for column alignment in text/CSV */
	int pmu_event_count;

	/* Summary (system-wide) data */
	struct summary_data summary;

	/* Per-CPU rows (tracked sampling set, dynamic allocation) */
	struct cpu_row *cpu_rows;
	int cpu_row_count;

	/* Flag: cpu_rows is a temp allocation (not from pool) */
	int cpu_rows_is_temp;

	/* Pointers to source data for serializers to access dynamic fields */
	const struct sys_snapshot *raw;
	const struct interval_stats *stats;
};

/*
 * Build interval_record from raw data
 */
struct interval_record *build_interval_record(
	const struct sys_snapshot *raw,
	const struct interval_stats *stats,
	int iteration);

/*
 * Free interval_record
 */
void free_interval_record(struct interval_record *rec);

/*
 * Text serializer
 */
void serialize_text(const struct interval_record *rec, int iteration);

/*
 * JSON serializer (stateless)
 */
void serialize_json(const struct interval_record *rec, int iteration);

/*
 * CSV serializer
 */
void serialize_csv(const struct interval_record *rec);

/*
 * Initialize formatter
 */
int init_formatter(void);

/*
 * Setup memory pool for interval_record
 * Called by main to pre-allocate based on expected CPU count
 */
void setup_formatter_pool(int max_cpus);

/*
 * Set output format
 */
void set_format(int format);

/*
 * Set quiet mode (no headers)
 */
void set_quiet(int quiet);

/*
 * Set summary mode (SUM only, no per-CPU)
 */
void set_summary_mode(int summary);
void set_default_summary_output(int enable);

/*
 * Set number of iterations (for footer control)
 */
void set_iterations(int iter);

/*
 * Set header print interval
 */
void set_header_interval(int interval);

/*
 * Enable/disable columns
 */
void enable_cpu(int enable);
void enable_freq(int enable);
void enable_idle(int enable);
void enable_iowait(int enable);
void update_idle_state_visibility(void);
int idle_state_columns_enabled(void);
void clear_idle_state_overrides(void);
void set_idle_state_override(int state_idx, int enable, int whitelist);
void enable_power(int enable);
void enable_temp(int enable);
void enable_pmu(int enable);
void enable_sysstat(int enable);
void enable_membw(int enable);

/* Extended columns */
void enable_numa(int enable);
void enable_package(int enable);
void enable_core(int enable);
void enable_ipc(int enable);
void enable_energy(int enable);
void update_temp_field_visibility(void);

/* Reset all columns to defaults */
void reset_columns(void);
void clear_columns(void);

/* CPU list filtering now lives in cpu_inventory; see cpu_inventory.h */

/*
 * Print interval header
 */
void print_interval_header(double interval);

/*
 * Print one interval of output
 *
 * @raw: Raw snapshot from collector
 * @stats: Aggregated statistics from aggregator
 * @iteration: Current iteration number (1-based)
 */
void print_interval(const struct sys_snapshot *raw,
		    const struct interval_stats *stats,
		    int iteration);

/*
 * List available counters
 */
void list_counters(void);

/*
 * Cleanup formatter
 */
void cleanup_formatter(void);

/*
 * Close output format (e.g., print JSON footer for infinite sampling mode)
 * Call this when exiting due to signal
 */
void close_format(const struct interval_stats *stats);

#endif /* ARMSTAT_FORMATTER_H */
