/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_machine.c - Shared machine-output helpers and stream lifecycle
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "formatter_machine.h"

unsigned long long machine_record_timestamp_ns(
	const struct interval_record *rec)
{
	if (rec->timestamp_ns)
		return rec->timestamp_ns;
	return (unsigned long long)rec->timestamp * 1000000000ULL;
}

void machine_format_timestamp_iso(const struct interval_record *rec,
				  char *buf, size_t buf_size)
{
	struct tm tm;
	char date[32];
	char zone[16];
	char raw_zone[16];
	unsigned long long timestamp_ns;
	int written;

	if (!buf_size)
		return;

	if (!localtime_r(&rec->timestamp, &tm)) {
		buf[0] = '\0';
		return;
	}

	if (strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm) == 0 ||
	    strftime(raw_zone, sizeof(raw_zone), "%z", &tm) == 0) {
		buf[0] = '\0';
		return;
	}
	if (strlen(raw_zone) == 5 &&
	    (raw_zone[0] == '+' || raw_zone[0] == '-')) {
		snprintf(zone, sizeof(zone), "%c%c%c:%c%c", raw_zone[0],
			 raw_zone[1], raw_zone[2], raw_zone[3], raw_zone[4]);
	} else {
		snprintf(zone, sizeof(zone), "%s", raw_zone);
	}

	timestamp_ns = machine_record_timestamp_ns(rec);
	written = snprintf(buf, buf_size, "%s.%09llu%s", date,
			   timestamp_ns % 1000000000ULL, zone);
	if (written < 0 || (size_t)written >= buf_size)
		buf[0] = '\0';
}

void machine_get_serialized_fields(enum field_scope scope,
				   struct field_desc **fields, int *count)
{
	int out = 0;

	get_enabled_fields(scope, fields, count);
	for (int i = 0; i < *count; i++) {
		if (!field_is_scope_identity(fields[i]))
			fields[out++] = fields[i];
	}
	*count = out;
}

void reset_machine_state(void)
{
	reset_json_state();
	reset_csv_state();
}
