/* SPDX-License-Identifier: GPL-2.0 */
/*
 * columns.h - Column visibility and field registry
 *
 * Owns two related concepts:
 *   - column visibility: the show_* flags + idle-state/temp-series override
 *     bitmasks that decide which fields appear in output
 *   - field registry: the all_fields[] descriptor table tying field ids to
 *     their group, scope, series, type, unit, precision, enabled flag, and
 *     value getter
 *
 * Written by armstat_cli.c (via the enable_* setters and override API) and
 * read by sample_cache.c (demand-driven sampling), the formatter_section
 * policy, and the serializers. formatter_values.c owns the value getters;
 * columns.c references them by address through the table.
 *
 * RESPONSIBLE FOR:
 *   - show_* group-visibility flags and their enable_* setters
 *   - idle-state and summary-temp series visibility + override bitmasks
 *   - idle-state label storage (updated from cpuidle state names)
 *   - the field descriptor table (all_fields[]) and its query API
 *   - field_is_effectively_enabled() — the one decision a reader should ask
 *
 * NOT RESPONSIBLE FOR:
 *   - reading sysfs/proc
 *   - building interval_record (lives in formatter_record.c)
 *   - the value getters themselves (defined in formatter_values.c, declared
 *     in the private formatter_fields.h sub-header)
 */

#ifndef ARMSTAT_COLUMNS_H
#define ARMSTAT_COLUMNS_H

#include <stdint.h>

/*
 * Field scope - defines where field data comes from
 */
enum field_scope {
	FIELD_SCOPE_SYSTEM,    /* System-wide: sampled freq, busy, power, etc. */
	FIELD_SCOPE_PACKAGE,   /* Per-package: sampled frequency, idle, busy, iowait */
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
 * split idle-state residency/usage or summary TempN values. Keep that identity
 * in field metadata so CLI selection and demand-driven collection do not have
 * to infer it from field ids.
 */
enum field_series {
	FIELD_SERIES_NONE = 0,
	FIELD_SERIES_IDLE_STATE_RESIDENCY,
	FIELD_SERIES_IDLE_STATE_USAGE,
	FIELD_SERIES_SUMMARY_TEMP,
};

static inline int field_series_is_idle_state(enum field_series series)
{
	return series == FIELD_SERIES_IDLE_STATE_RESIDENCY ||
	       series == FIELD_SERIES_IDLE_STATE_USAGE;
}

/*
 * Field type - defines the data type of a field
 */
enum field_type {
	FIELD_TYPE_INT,
	FIELD_TYPE_LLONG,
	FIELD_TYPE_DOUBLE,
	FIELD_TYPE_STRING,
	FIELD_TYPE_BOOL,
};

/*
 * Field getter functions - used for data-driven serialization
 */
struct interval_record;
struct cpu_row;
struct summary_data;

/* Getter function types.
 * The int parameter is a row index within the field's scope iteration:
 * a tracked CPU index for CPU-scope fields, a package index for
 * package-scope fields, and ignored for system-scope fields.
 */
typedef double (*getter_double)(const struct interval_record *rec, int row_idx);
typedef long long (*getter_llong)(const struct interval_record *rec, int row_idx);
typedef int (*getter_int)(const struct interval_record *rec, int row_idx);
typedef const char *(*getter_string)(const struct interval_record *rec, int row_idx);

/*
 * Field descriptor - metadata for a single field
 * This enables centralized field management instead of scattered printf logic
 */
struct field_desc {
	const char *id;           /* Stable CLI field ID: "summary_freq_mhz", "freq_mhz" */
	const char *label;        /* Column header: "Freq", "cpu0_cycles" */
	const char *json_label;   /* JSON label (can differ from id) */
	const char *unit;         /* Human-readable unit, empty if dimensionless */
	enum field_scope scope;   /* Data scope */
	enum field_type type;     /* Data type */
	int decimals;             /* Decimal places for floating-point output */
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
 * written by armstat_cli.c CLI parsing via the enable_*() functions below.
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
static inline int is_idle_enabled(void)    { return show_idle; }
static inline int is_iowait_enabled(void)  { return show_iowait; }
static inline int is_power_enabled(void)   { return show_power; }
static inline int is_temp_enabled(void)    { return show_temp; }
static inline int is_sysstat_enabled(void) { return show_sysstat; }
static inline int is_energy_enabled(void)  { return show_energy; }
static inline int is_membw_enabled(void)   { return show_membw; }
static inline int is_pmu_enabled(void)     { return show_pmu; }
static inline int is_ipc_enabled(void)     { return show_ipc; }

/* Number of idle-state columns exposed at most (LPI-0 ... LPI-N) */
#define MAX_VISIBLE_IDLE_STATES 8

/*
 * Idle-state series visibility (read by the record builder to apply the
 * residual display rule during materialization). Written by
 * update_idle_state_visibility() based on cpuidle state count + overrides.
 */
extern int show_idle_state[MAX_VISIBLE_IDLE_STATES];
extern int show_summary_idle_state[MAX_VISIBLE_IDLE_STATES];

/*
 * Field registry query API.
 */
struct field_desc *get_field_desc(const char *field_id);
struct field_desc *get_all_fields(void);
int get_field_count(void);
int any_fields_enabled(enum field_scope scope);
void get_enabled_fields(enum field_scope scope, struct field_desc **fields, int *count);
unsigned int get_enabled_series_mask(enum field_scope scope,
				     enum field_series series);
int field_is_scope_identity(const struct field_desc *field);
void clear_field_overrides(void);
void set_field_override_by_index(int field_index, int enable, int whitelist);

/*
 * Idle-state series visibility + overrides.
 */
void update_idle_state_visibility(void);
void clear_idle_state_overrides(void);
void set_idle_state_override(int state_idx, int enable, int whitelist);

/*
 * Enable/disable column groups (written by CLI parsing).
 */
void enable_cpu(int enable);
void enable_freq(int enable);
void enable_idle(int enable);
void enable_iowait(int enable);
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

/* Reset all columns to defaults / clear all */
void reset_columns(void);
void clear_columns(void);

/*
 * Format a field value into buf as a string. The nan_str parameter controls
 * how unavailable values are rendered (e.g. "" for CSV, "-" for text).
 * Available doubles use the precision declared by the descriptor. Shared by
 * the CSV and text serializers; the JSON serializer keeps its own
 * print_json_field_value because it needs stream-based string escaping.
 */
void format_field_value(const struct field_desc *field,
			const struct interval_record *rec,
			int row_idx,
			const char *nan_str,
			char *buf, size_t buf_size);

#endif /* ARMSTAT_COLUMNS_H */
