/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_POWER_INTERNAL_H
#define ARMSTAT_POWER_INTERNAL_H

/* Private wiring shared by the power facade and its implementation modules. */
int init_power_sensor_subsystem(void);
void close_power_sensor_subsystem(void);
int init_mem_bw(void);
void close_mem_bw(void);

#endif /* ARMSTAT_POWER_INTERNAL_H */
