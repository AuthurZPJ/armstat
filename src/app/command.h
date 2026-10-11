/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ARMSTAT_COMMAND_H
#define ARMSTAT_COMMAND_H

/* Start the measurement while the child is gated, then run and reap it.
 * Returns its shell-style exit status, or -1 for a monitor/setup failure.
 * child_stdout_fd preserves the caller's stdout when statistics are redirected.
 */
int run_monitored_command(char *const argv[], int child_stdout_fd,
			  int (*start_measurement)(void *), void *context);

#endif /* ARMSTAT_COMMAND_H */
