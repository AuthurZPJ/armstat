/* SPDX-License-Identifier: GPL-2.0 */
/*
 * columns.c - Column visibility and field registry
 *
 * Owns the show_* group-visibility flags, the idle-state and summary-temp
 * series visibility + override bitmasks, the idle-state label storage, and
 * the all_fields[] descriptor table. The value getters referenced by the
 * table are defined in formatter_values.c and declared in the private
 * formatter_fields.h sub-header.
 *
 * See columns.h for the interface contract.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "columns.h"
#include "formatter_fields.h"
#include "cpufreq.h"
#include "cpuidle.h"
#include "power.h"

#define ARRAY_SIZE(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

/* ============================================================================
 * COLUMN VISIBILITY FLAGS
 * ============================================================================ */

/* Column visibility flags - exposed to main for -s/-H options */
int show_cpu = 1;
int show_freq = 1;
int show_idle = 1;
int show_iowait = 0;
int show_power = 1;
int show_temp = 1;
int show_pmu = 0;
int show_sysstat = 0;
int show_membw = 0;
int show_package = 0;
int show_core = 0;
int show_numa = 0;
int show_ipc = 0;
int show_energy = 0;

/* Idle state visibility - dynamically set based on actual state count */
int show_idle_state[8] = {1, 1, 0, 0, 0, 0, 0, 0};

/* Summary idle state visibility - same as CPU idle but for SUM row */
int show_summary_idle_state[8] = {1, 1, 0, 0, 0, 0, 0, 0};

/*
 * Idle state override bitmasks:
 *   idle_show_mask: bit set -> explicitly shown
 *   idle_hide_mask: bit set -> explicitly hidden
 *   both clear      -> inherit (available -> visible)
 *
 * If any show bit is set, whitelist semantics apply: only explicitly
 * shown states are visible.
 */
static unsigned int idle_show_mask;
static unsigned int idle_hide_mask;

/* Idle state labels - updated from cpuidle state names when available */
static char idle_state_labels[8][16] = {
	"LPI-0", "LPI-1", "LPI-2", "LPI-3",
	"LPI-4", "LPI-5", "LPI-6", "LPI-7"
};
static char idle_state_wakeup_labels[8][24] = {
	"LPI-0_wake", "LPI-1_wake", "LPI-2_wake", "LPI-3_wake",
	"LPI-4_wake", "LPI-5_wake", "LPI-6_wake", "LPI-7_wake"
};

static void set_default_idle_state_labels(void)
{
	for (int i = 0; i < ARRAY_SIZE(idle_state_labels); i++) {
		snprintf(idle_state_labels[i], sizeof(idle_state_labels[i]), "LPI-%d", i);
		snprintf(idle_state_wakeup_labels[i],
			 sizeof(idle_state_wakeup_labels[i]), "LPI-%d_wake", i);
	}
}

static void set_idle_state_columns_visible(int count)
{
	for (int i = 0; i < ARRAY_SIZE(show_idle_state); i++) {
		int enabled = (i < count);
		show_idle_state[i] = enabled;
		show_summary_idle_state[i] = enabled;
	}
}

static void clear_idle_state_columns(void)
{
	set_idle_state_columns_visible(0);
}

static void reset_idle_state_overrides_internal(void)
{
	idle_show_mask = 0;
	idle_hide_mask = 0;
}

void clear_idle_state_overrides(void)
{
	reset_idle_state_overrides_internal();
}

void set_idle_state_override(int state_idx, int enable, int whitelist)
{
	unsigned int bit;

	(void)whitelist;
	if (state_idx < 0 || state_idx >= 8)
		return;

	bit = 1U << state_idx;
	if (enable) {
		idle_show_mask |= bit;
		idle_hide_mask &= ~bit;
	} else {
		idle_hide_mask |= bit;
		idle_show_mask &= ~bit;
	}
}

/* ============================================================================
 * FIELD TABLE MACROS
 * ============================================================================ */

#define SUMMARY_IDLE_FIELD(idx)							\
	{"sum_idle_state" #idx, idle_state_labels[idx], "lpi" #idx, "%",	\
	 FIELD_SCOPE_SYSTEM, FIELD_TYPE_DOUBLE, 2, FIELD_GROUP_IDLE,		\
	 FIELD_SERIES_IDLE_STATE, idx,					\
	 &show_summary_idle_state[idx],					\
	 .getter.get_double = get_summary_idle_state##idx}

#define TEMP_VDIE_FIELD(idx)							\
	{"temp_vdie" #idx, "Temp" #idx, "temp" #idx, "degC",		\
	 FIELD_SCOPE_SYSTEM, FIELD_TYPE_DOUBLE, 2, FIELD_GROUP_TEMP,		\
	 FIELD_SERIES_SUMMARY_TEMP, idx,					\
	 &show_temp,							\
	 .getter.get_double = get_temp_vdie##idx}

#define CPU_IDLE_WAKEUP_FIELD(idx)						\
	{"idle_state_wakeup" #idx, idle_state_wakeup_labels[idx], "lpi" #idx "_wake", "/s", \
	 FIELD_SCOPE_CPU, FIELD_TYPE_DOUBLE, 2, FIELD_GROUP_IDLE,	\
	 FIELD_SERIES_IDLE_STATE, idx,					\
	 &show_idle_state[idx],						\
	 .getter.get_double = get_cpu_idle_state_wakeup##idx}

#define CPU_IDLE_FIELD(idx)							\
	{"idle_state" #idx, idle_state_labels[idx], "lpi" #idx, "%",		\
	 FIELD_SCOPE_CPU, FIELD_TYPE_DOUBLE, 2, FIELD_GROUP_IDLE,		\
	 FIELD_SERIES_IDLE_STATE, idx,					\
	 &show_idle_state[idx],						\
	 .getter.get_double = get_cpu_idle_state##idx}

#define SYSTEM_DOUBLE_FIELD(id, label, json_label, unit_name, precision, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, unit_name, FIELD_SCOPE_SYSTEM, FIELD_TYPE_DOUBLE, precision, \
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_double = getter_fn }

#define SYSTEM_LLONG_FIELD(id, label, json_label, unit_name, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, unit_name, FIELD_SCOPE_SYSTEM, FIELD_TYPE_LLONG, 0, \
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_llong = getter_fn }

#define CPU_INT_FIELD(id, label, json_label, unit_name, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, unit_name, FIELD_SCOPE_CPU, FIELD_TYPE_INT, 0, \
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_int = getter_fn }

#define CPU_DOUBLE_FIELD(id, label, json_label, unit_name, precision, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, unit_name, FIELD_SCOPE_CPU, FIELD_TYPE_DOUBLE, precision, \
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_double = getter_fn }

#define CPU_STRING_FIELD(id, label, json_label, unit_name, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, unit_name, FIELD_SCOPE_CPU, FIELD_TYPE_STRING, 0, \
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_string = getter_fn }

#define CPU_BOOL_FIELD(id, label, json_label, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, "", FIELD_SCOPE_CPU, FIELD_TYPE_BOOL, 0, \
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_int = getter_fn }

#define PACKAGE_DOUBLE_FIELD(id, label, json_label, unit_name, precision, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, unit_name, FIELD_SCOPE_PACKAGE, FIELD_TYPE_DOUBLE, precision, \
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_double = getter_fn }

#define PACKAGE_INT_FIELD(id, label, json_label, unit_name, group_mask, enabled_ptr, getter_fn) \
	{ id, label, json_label, unit_name, FIELD_SCOPE_PACKAGE, FIELD_TYPE_INT, 0, \
	  group_mask, FIELD_SERIES_NONE, -1,				\
	  enabled_ptr, .getter.get_int = getter_fn }

/* ============================================================================
 * FIELD DESCRIPTOR TABLE
 * ============================================================================ */

struct field_desc all_fields[] = {
	/* Per-package fields */
	PACKAGE_INT_FIELD("pkg_id", "Pkg", "package", "", FIELD_GROUP_PACKAGE,
			  &show_package, get_pkg_package_id),
	PACKAGE_DOUBLE_FIELD("pkg_avg_freq", "Freq", "freq", "MHz", 2, FIELD_GROUP_FREQ,
			 &show_freq, get_pkg_avg_mhz),
	PACKAGE_DOUBLE_FIELD("pkg_idle_percent", "Idle%", "idle_percent", "%", 2, FIELD_GROUP_IDLE,
			 &show_idle, get_pkg_idle_percent),
	PACKAGE_DOUBLE_FIELD("pkg_busy_percent", "Busy%", "busy_percent", "%", 2, FIELD_GROUP_IDLE,
			 &show_idle, get_pkg_busy_percent),
	PACKAGE_DOUBLE_FIELD("pkg_iowait_percent", "IOWait%", "iowait_percent", "%", 2, FIELD_GROUP_IDLE,
			 &show_iowait, get_pkg_iowait_percent),
	PACKAGE_INT_FIELD("pkg_cpu_count", "CPUs", "cpu_count", "count", FIELD_GROUP_PACKAGE,
		      &show_package, get_pkg_cpu_count),

	/* System-wide fields (shown in SUM row) */
	SYSTEM_DOUBLE_FIELD("avg_mhz", "AvgFreq", "avg_freq", "MHz", 2, FIELD_GROUP_FREQ,
			    &show_freq, get_summary_avg_mhz),
	SYSTEM_DOUBLE_FIELD("uncore_freq", "UncoreFreq", "uncore_freq", "MHz", 2, FIELD_GROUP_FREQ,
			    &show_freq, get_summary_uncore_freq_mhz),
	/* Summary idle state percentages - show before busy%/idle% */
	SUMMARY_IDLE_FIELD(0),
	SUMMARY_IDLE_FIELD(1),
	SUMMARY_IDLE_FIELD(2),
	SUMMARY_IDLE_FIELD(3),
	SUMMARY_IDLE_FIELD(4),
	SUMMARY_IDLE_FIELD(5),
	SUMMARY_IDLE_FIELD(6),
	SUMMARY_IDLE_FIELD(7),
	SYSTEM_DOUBLE_FIELD("idle_percent", "Idle%", "idle_percent", "%", 2, FIELD_GROUP_IDLE,
			    &show_idle, get_summary_idle_percent),
	SYSTEM_DOUBLE_FIELD("iowait_percent", "IOWait%", "iowait_percent", "%", 2, FIELD_GROUP_IDLE,
			    &show_iowait, get_summary_iowait_percent),
	SYSTEM_DOUBLE_FIELD("busy_percent", "Busy%", "busy_percent", "%", 2, FIELD_GROUP_IDLE,
			    &show_idle, get_summary_busy_percent),
	SYSTEM_DOUBLE_FIELD("power_mw", "Power", "power", "mW", 2, FIELD_GROUP_POWER,
			    &show_power, get_summary_power_mw),
	TEMP_VDIE_FIELD(0),
	TEMP_VDIE_FIELD(1),
	TEMP_VDIE_FIELD(2),
	TEMP_VDIE_FIELD(3),
	SYSTEM_DOUBLE_FIELD("energy_joules", "Energy", "energy", "J", 2, FIELD_GROUP_ENERGY,
			    &show_energy, get_summary_energy_joules),
	SYSTEM_DOUBLE_FIELD("mem_bw", "MemBW", "mem_bw", "MiB/s", 2, FIELD_GROUP_MEMBW,
			    &show_membw, get_summary_mem_bw),
	SYSTEM_DOUBLE_FIELD("ctx_switches", "CtxSw", "ctx_switches", "count/interval", 0, FIELD_GROUP_SYSSTAT,
			    &show_sysstat, get_summary_ctx_switches),
	SYSTEM_DOUBLE_FIELD("interrupts", "IRQs", "interrupts", "count/interval", 0, FIELD_GROUP_SYSSTAT,
			    &show_sysstat, get_summary_interrupts),
	SYSTEM_DOUBLE_FIELD("soft_interrupts", "SoftIRQs", "soft_interrupts", "count/interval", 0, FIELD_GROUP_SYSSTAT,
			    &show_sysstat, get_summary_soft_interrupts),
	SYSTEM_DOUBLE_FIELD("ipc", "IPC", "ipc", "instructions/cycle", 2, FIELD_GROUP_IPC,
			    &show_ipc, get_summary_ipc),

	/* Per-CPU fields (excluding 'cpu' which is printed manually in serializers) */
	CPU_INT_FIELD("package", "Pkg", "package", "", FIELD_GROUP_PACKAGE,
		      &show_package, get_cpu_package),
	CPU_INT_FIELD("core", "Core", "core", "", FIELD_GROUP_CORE,
		      &show_core, get_cpu_core),
	CPU_INT_FIELD("numa_node", "Node", "node", "", FIELD_GROUP_NUMA,
		      &show_numa, get_cpu_numa_node),
	CPU_DOUBLE_FIELD("freq_mhz", "Freq", "freq", "MHz", 2, FIELD_GROUP_FREQ,
			 &show_freq, get_cpu_freq_mhz),
	CPU_DOUBLE_FIELD("min_freq_mhz", "Min", "min", "MHz", 2, FIELD_GROUP_FREQ,
			 &show_freq, get_cpu_min_freq_mhz),
	CPU_DOUBLE_FIELD("max_freq_mhz", "Max", "max", "MHz", 2, FIELD_GROUP_FREQ,
			 &show_freq, get_cpu_max_freq_mhz),
	CPU_STRING_FIELD("governor", "Governor", "governor", "", FIELD_GROUP_FREQ,
			 &show_freq, get_cpu_governor),
	CPU_BOOL_FIELD("boost", "Boost", "boost", FIELD_GROUP_FREQ,
			 &show_freq, get_cpu_boost),
	/* Per-idle-state percentages - show before busy%/idle% */
	CPU_IDLE_FIELD(0),
	CPU_IDLE_FIELD(1),
	CPU_IDLE_FIELD(2),
	CPU_IDLE_FIELD(3),
	CPU_IDLE_FIELD(4),
	CPU_IDLE_FIELD(5),
	CPU_IDLE_FIELD(6),
	CPU_IDLE_FIELD(7),
	CPU_IDLE_WAKEUP_FIELD(0),
	CPU_IDLE_WAKEUP_FIELD(1),
	CPU_IDLE_WAKEUP_FIELD(2),
	CPU_IDLE_WAKEUP_FIELD(3),
	CPU_IDLE_WAKEUP_FIELD(4),
	CPU_IDLE_WAKEUP_FIELD(5),
	CPU_IDLE_WAKEUP_FIELD(6),
	CPU_IDLE_WAKEUP_FIELD(7),
	CPU_DOUBLE_FIELD("cpu_idle_percent", "Idle%", "idle_percent", "%", 2, FIELD_GROUP_IDLE,
			 &show_idle, get_cpu_idle_percent),
	CPU_DOUBLE_FIELD("cpu_iowait_percent", "IOWait%", "iowait_percent", "%", 2, FIELD_GROUP_IDLE,
			 &show_iowait, get_cpu_iowait_percent),
	CPU_DOUBLE_FIELD("cpu_busy_percent", "Busy%", "busy_percent", "%", 2, FIELD_GROUP_IDLE,
			 &show_idle, get_cpu_busy_percent),
	CPU_DOUBLE_FIELD("cpu_ipc", "IPC", "ipc", "instructions/cycle", 2, FIELD_GROUP_IPC,
			 &show_ipc, get_cpu_ipc),
	/* Note: cpu_power_mw removed - power only shown at SUM level */
	CPU_DOUBLE_FIELD("cpu_temp_c", "Temp", "temp", "degC", 2, FIELD_GROUP_TEMP,
			 &show_temp, get_cpu_temp_c),
};

#define NUM_FIELDS (int)(sizeof(all_fields) / sizeof(all_fields[0]))
_Static_assert(NUM_FIELDS <= 64,
	       "NUM_FIELDS exceeds uint64_t bitmask capacity");

/*
 * Field override bitmasks.
 *
 * field_show_mask: bit set -> explicitly show this field
 * field_hide_mask: bit set -> explicitly hide this field
 * both clear: inherit from group flag (show_* variable)
 *
 * If any show bit is set, whitelist semantics apply: only fields with
 * their show bit set are visible.
 */
static uint64_t field_show_mask;
static uint64_t field_hide_mask;

#undef SUMMARY_IDLE_FIELD
#undef TEMP_VDIE_FIELD
#undef CPU_IDLE_WAKEUP_FIELD
#undef CPU_IDLE_FIELD
#undef SYSTEM_DOUBLE_FIELD
#undef SYSTEM_LLONG_FIELD
#undef CPU_INT_FIELD
#undef CPU_DOUBLE_FIELD
#undef CPU_STRING_FIELD
#undef CPU_BOOL_FIELD
#undef PACKAGE_DOUBLE_FIELD
#undef PACKAGE_INT_FIELD

static int field_is_effectively_enabled(int field_index)
{
	const struct field_desc *field;

	if (field_index < 0 || field_index >= NUM_FIELDS)
		return 0;

	field = &all_fields[field_index];

	/*
	 * Uncore frequency is part of the frequency group, but it should only
	 * take a default column slot when the platform actually exposes a usable
	 * devfreq-backed uncore source.
	 */
	if (field->id && strcmp(field->id, "uncore_freq") == 0 &&
	    !has_uncore_freq_support())
		return 0;

	if (field->series == FIELD_SERIES_IDLE_STATE &&
	    !*field->enabled_ptr)
		return 0;

	/*
	 * Summary temperature availability is orthogonal to selection. The group
	 * flag (or an exact-field override) decides whether the user selected the
	 * field; the discovered sparse sensor mask decides whether it can exist on
	 * this platform. Keeping those decisions separate prevents a runtime
	 * visibility refresh from erasing an explicit `-s TempN` request.
	 */
	if (field->series == FIELD_SERIES_SUMMARY_TEMP &&
	    (field->series_index < 0 || field->series_index >= 4 ||
	     !(get_temp_numa_mask() & (1U << field->series_index))))
		return 0;

	if (field->id && strcmp(field->id, "cpu_temp_c") == 0 &&
	    get_temp_numa_count() <= 0)
		return 0;

	if (field_hide_mask & (1ULL << field_index))
		return 0;
	if (field_show_mask & (1ULL << field_index))
		return 1;
	if (field_show_mask)
		return 0;  /* whitelist: only explicitly shown fields */
	return *field->enabled_ptr;
}

void clear_field_overrides(void)
{
	field_show_mask = 0;
	field_hide_mask = 0;
}

void set_field_override_by_index(int field_index, int enable, int whitelist)
{
	uint64_t bit;

	(void)whitelist;
	if (field_index < 0 || field_index >= NUM_FIELDS)
		return;

	bit = 1ULL << field_index;
	if (enable) {
		field_show_mask |= bit;
		field_hide_mask &= ~bit;
	} else {
		field_hide_mask |= bit;
		field_show_mask &= ~bit;
	}
}

/*
 * Get field descriptor by ID
 */
struct field_desc *get_field_desc(const char *field_id)
{
	for (int i = 0; i < NUM_FIELDS; i++) {
		if (strcmp(all_fields[i].id, field_id) == 0)
			return &all_fields[i];
	}
	return NULL;
}

/*
 * Get all fields
 */
struct field_desc *get_all_fields(void)
{
	return all_fields;
}

int get_field_count(void)
{
	return NUM_FIELDS;
}

/*
 * Check if any fields of a given scope are enabled
 */
int any_fields_enabled(enum field_scope scope)
{
	for (int i = 0; i < NUM_FIELDS; i++) {
		if (all_fields[i].scope == scope && field_is_effectively_enabled(i))
			return 1;
	}
	return 0;
}

/*
 * Get enabled fields for a scope
 */
void get_enabled_fields(enum field_scope scope, struct field_desc **fields, int *count)
{
	*count = 0;
	for (int i = 0; i < NUM_FIELDS; i++) {
		if (all_fields[i].scope == scope && field_is_effectively_enabled(i)) {
			fields[(*count)++] = &all_fields[i];
		}
	}
}

int field_is_scope_identity(const struct field_desc *field)
{
	return field && field->scope == FIELD_SCOPE_PACKAGE && field->id &&
	       strcmp(field->id, "pkg_id") == 0;
}

/* ============================================================================
 * COLUMN ENABLE/DISABLE API
 * ============================================================================ */

/*
 * Update idle state visibility based on actual state count.
 * Call this after cpuidle is initialized to show only existing states.
 */
void update_idle_state_visibility(void)
{
	int state_count = get_global_idle_state_count();

	/* If cpuidle is not enabled/available, hide per-state residency columns entirely. */
	if (!is_cpuidle_enabled() || state_count <= 0) {
		clear_idle_state_columns();
		set_default_idle_state_labels();
		return;
	}

	for (int i = 0; i < ARRAY_SIZE(show_idle_state); i++) {
		const char *name = get_idle_state_name(i);
		int available = (i < state_count);
		int enabled;

		if (name && *name) {
			snprintf(idle_state_labels[i], sizeof(idle_state_labels[i]), "%s", name);
		} else {
			snprintf(idle_state_labels[i], sizeof(idle_state_labels[i]), "LPI-%d", i);
		}
		snprintf(idle_state_wakeup_labels[i],
			 sizeof(idle_state_wakeup_labels[i]), "%.*s_wake",
			 (int)(sizeof(idle_state_wakeup_labels[i]) -
			       sizeof("_wake")),
			 idle_state_labels[i]);

		if (!available) {
			enabled = 0;
		} else if (idle_hide_mask & (1U << i)) {
			enabled = 0;
		} else if (idle_show_mask & (1U << i)) {
			enabled = 1;
		} else if (idle_show_mask) {
			enabled = 0;  /* whitelist: only explicitly shown states */
		} else {
			enabled = 1;
		}

		show_idle_state[i] = enabled;
		show_summary_idle_state[i] = enabled;
	}
}

int idle_state_columns_enabled(void)
{
	for (int i = 0; i < ARRAY_SIZE(show_idle_state); i++) {
		if (show_idle_state[i] || show_summary_idle_state[i])
			return 1;
	}
	return 0;
}

void enable_cpu(int e)    { show_cpu    = e; }
void enable_freq(int e)   { show_freq   = e; }
void enable_idle(int e)
{
	show_idle = e;
	if (e)
		update_idle_state_visibility();
	else
		clear_idle_state_columns();
}
void enable_iowait(int e) { show_iowait = e; }
void enable_power(int e)  { show_power  = e; }
void enable_temp(int e)   { show_temp   = e; }
void enable_pmu(int e)    { show_pmu    = e; }
void enable_sysstat(int e){ show_sysstat = e; }
void enable_membw(int e)  { show_membw  = e; }
void enable_numa(int e)   { show_numa   = e; }
void enable_package(int e){ show_package = e; }
void enable_core(int e)   { show_core   = e; }
void enable_ipc(int e)    { show_ipc    = e; }
void enable_energy(int e) { show_energy = e; }

void update_temp_field_visibility(void)
{
	/* Availability is evaluated lazily by field_is_effectively_enabled(). */
}

static void set_all_show_flags(int val)
{
	show_cpu    = val;
	show_freq   = val;
	show_idle   = val;
	show_iowait = 0;
	show_power  = val;
	show_temp   = val;
	show_pmu    = 0;
	show_sysstat= 0;
	show_membw  = 0;
	show_package= 0;
	show_core   = 0;
	show_numa   = 0;
	show_ipc    = 0;
	show_energy = 0;
}

void reset_columns(void)
{
	set_all_show_flags(1);
	update_temp_field_visibility();
	clear_field_overrides();
	reset_idle_state_overrides_internal();
	set_idle_state_columns_visible(2); /* default: first 2 states */
}

void clear_columns(void)
{
	set_all_show_flags(0);
	update_temp_field_visibility();
	clear_field_overrides();
	reset_idle_state_overrides_internal();
	clear_idle_state_columns();
}

/* ============================================================================
 * FIELD VALUE FORMATTING
 * ============================================================================ */

void format_field_value(const struct field_desc *field,
			const struct interval_record *rec,
			int row_idx,
			const char *nan_str,
			char *buf, size_t buf_size)
{
	if (!buf || buf_size == 0)
		return;

	switch (field->type) {
	case FIELD_TYPE_DOUBLE: {
		double val = field->getter.get_double(rec, row_idx);
		if (!isfinite(val))
			snprintf(buf, buf_size, "%s", nan_str ? nan_str : "");
		else
			snprintf(buf, buf_size, "%.*f", field->decimals, val);
		break;
	}
	case FIELD_TYPE_LLONG:
		snprintf(buf, buf_size, "%lld", field->getter.get_llong(rec, row_idx));
		break;
	case FIELD_TYPE_INT:
		snprintf(buf, buf_size, "%d", field->getter.get_int(rec, row_idx));
		break;
	case FIELD_TYPE_STRING: {
		const char *val = field->getter.get_string(rec, row_idx);
		snprintf(buf, buf_size, "%s",
			 val ? val : (nan_str ? nan_str : ""));
		break;
	}
	case FIELD_TYPE_BOOL: {
		int val = field->getter.get_int(rec, row_idx);

		if (val < 0)
			snprintf(buf, buf_size, "%s", nan_str ? nan_str : "");
		else
			snprintf(buf, buf_size, "%d", !!val);
		break;
	}
	default:
		buf[0] = '\0';
		break;
	}
}
