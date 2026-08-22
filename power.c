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
#include "power_internal.h"

/*
 * Initialize power subsystem
 * Delegates to power_sensor for initialization
 */
int init_power(void)
{
	if (init_power_sensor_subsystem() < 0)
		return -1;

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
