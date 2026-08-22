/* SPDX-License-Identifier: GPL-2.0 */
/*
 * power_interval.c - Interval-based power and energy calculations
 *
 * Responsibilities:
 *   - Calculate avg_power from readings
 *   - Energy integration over time
 *
 * Does NOT handle:
 *   - Sensor discovery (power_sensor.c)
 *   - Memory bandwidth (membw.c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "power.h"

/* ============================================================================
 * STATE
 * ============================================================================ */

/* Interval-based calculated values */
static double interval_avg_power_mw;         /* Average power during interval (mW) */
static double interval_energy_joules;        /* Energy consumed during last interval (Joules) */

/* Energy tracking */
static int energy_initialized;
static int power_gap;

/* Power delta tracking */
static unsigned long long prev_power_reading;  /* Previous total power for delta */

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

/*
 * Reset energy counters
 */
void reset_energy(void)
{
	interval_energy_joules = NAN;
	energy_initialized = 0;
	interval_avg_power_mw = NAN;
	prev_power_reading = 0;
	power_gap = 0;
}

/*
 * Update interval statistics with current power reading
 *
 * @delta_us: time elapsed since last update (microseconds)
 * @current_power: current total power reading (milliwatts)
 */
void update_power_interval_stats(unsigned long long delta_us,
				 unsigned long long current_power,
				 int current_valid)
{
	double avg_mw;

	if (!current_valid) {
		interval_avg_power_mw = NAN;
		interval_energy_joules = NAN;
		energy_initialized = 0;
		power_gap = 1;
		return;
	}

	/* First call or recovery - initialize without bridging a missing sample. */
	if (!energy_initialized || delta_us == 0) {
		prev_power_reading = current_power;
		if (delta_us == 0 && !power_gap) {
			interval_avg_power_mw = (double)current_power;
			interval_energy_joules = 0.0;
		} else {
			interval_avg_power_mw = NAN;
			interval_energy_joules = NAN;
		}
		energy_initialized = 1;
		power_gap = 0;
		return;
	}

	/*
	 * Treat power as a sampled signal and integrate with a trapezoid over the
	 * shared collector interval. This gives us a real interval average instead
	 * of simply relabeling the current reading as "avg power".
	 */
	avg_mw = (double)prev_power_reading / 2.0 +
		(double)current_power / 2.0;
	interval_avg_power_mw = avg_mw;

	/* Calculate energy: E = P * t
	 * Power in mW, time in us -> energy in mJ = mW * us / 1000000
	 * Convert to Joules: mJ / 1000 = J */
	double delta_s = (double)delta_us / 1000000.0;
	interval_energy_joules = (avg_mw * delta_s) / 1000.0;

	prev_power_reading = current_power;
}

/*
 * Get average power for the interval
 */
double get_interval_avg_power_mw(void)
{
	return interval_avg_power_mw;
}

/*
 * Get energy for the last interval (in Joules)
 */
double get_interval_energy_joules(void)
{
	return interval_energy_joules;
}
