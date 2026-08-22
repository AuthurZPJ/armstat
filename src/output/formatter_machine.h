/* SPDX-License-Identifier: GPL-2.0 */
/*
 * formatter_machine.h - Private contract shared by JSON/CSV serializers
 */

#ifndef ARMSTAT_FORMATTER_MACHINE_H
#define ARMSTAT_FORMATTER_MACHINE_H

#include <stddef.h>

#include "formatter.h"

#define MACHINE_SCHEMA_VERSION 7
#define MACHINE_FIELD_CAPACITY 64

unsigned long long machine_record_timestamp_ns(
	const struct interval_record *rec);
void machine_format_timestamp_iso(const struct interval_record *rec,
				  char *buf, size_t buf_size);
void machine_get_serialized_fields(enum field_scope scope,
				   struct field_desc **fields, int *count);

void reset_json_state(void);
void reset_csv_state(void);

#endif /* ARMSTAT_FORMATTER_MACHINE_H */
