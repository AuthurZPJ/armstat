/* SPDX-License-Identifier: GPL-2.0 */
/*
 * power.c - Power and temperature monitoring (facade)
 *
 * This file delegates to specialized modules:
 *   - power_sensor.c: Sensor discovery and raw reading
 *   - power_interval.c: Interval-based power/energy calculations
 *   - membw.c: Memory bandwidth tracking
 *
 * Responsibilities:
 *   - Coordinate between sub-modules
 *   - Provide backward-compatible API
 *
 * Does NOT handle:
 *   - Sensor scanning (power_sensor.c)
 *   - Power calculations (power_interval.c)
 *   - Memory bandwidth (membw.c)
 */

#include "power.h"

/* Internal wiring — previously in power_internal.h */
int  init_power_sensor_subsystem(void);
void close_power_sensor_subsystem(void);
int  init_mem_bw(void);
void close_mem_bw(void);

/*
 * Initialize power subsystem
 * Delegates to power_sensor for initialization
 */
int init_power(void)
{
	init_power_sensor_subsystem();

	/* Initialize memory bandwidth subsystem */
	init_mem_bw();
	reset_mem_bw();

	/* Reset energy counters */
	reset_energy();

	return 0;
}

/*
 * Close power subsystem
 */
void close_power(void)
{
	close_power_sensor_subsystem();

	close_mem_bw();
}
