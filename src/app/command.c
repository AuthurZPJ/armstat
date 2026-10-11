/* SPDX-License-Identifier: GPL-2.0 */
/* Command lifecycle, isolated from telemetry for host-side validation. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "command.h"

static volatile sig_atomic_t command_pid;
static volatile sig_atomic_t resume_requested;

static void forward_signal(int sig)
{
	int saved_errno = errno;
	pid_t pid = (pid_t)command_pid;

	if (sig == SIGCONT)
		resume_requested = 1;
	else if (sig != SIGCHLD && pid > 0)
		kill(-pid, sig);
	errno = saved_errno;
}

static int cloexec_pipe(int fds[2])
{
	if (pipe(fds) < 0)
		return -1;
	if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
	    fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
		int saved_errno = errno;

		close(fds[0]);
		close(fds[1]);
		errno = saved_errno;
		return -1;
	}
	return 0;
}

static void child_error(int fd, int error)
{
	ssize_t written;

	do {
		written = write(fd, &error, sizeof(error));
	} while (written < 0 && errno == EINTR);
	_exit(error == ENOENT ? 127 : 126);
}

int run_monitored_command(char *const argv[], int child_stdout_fd,
			  int (*start_measurement)(void *), void *context)
{
	const int signals[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCONT, SIGCHLD };
	struct sigaction old_actions[6], old_ttou, action;
	sigset_t blocked, old_mask, wait_mask;
	int gate[2] = { -1, -1 }, errors[2] = { -1, -1 };
	int installed = 0, ttou_installed = 0, mask_saved = 0;
	int tty_fd = -1, foreground = 0, status = 0, result = -1;
	int exec_error = 0, saved_errno = 0;
	pid_t pid = -1, child_group = -1;
	char token = 's';
	ssize_t n;

	if (!argv || !argv[0] || !start_measurement) {
		errno = EINVAL;
		return -1;
	}
	if (cloexec_pipe(gate) < 0 || cloexec_pipe(errors) < 0)
		goto failure;
	sigemptyset(&blocked);
	for (unsigned int i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
		sigaddset(&blocked, signals[i]);
	if (sigprocmask(SIG_BLOCK, &blocked, &old_mask) < 0)
		goto failure;
	mask_saved = 1;
	wait_mask = old_mask;
	for (unsigned int i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
		sigdelset(&wait_mask, signals[i]);
	resume_requested = 0;
	memset(&action, 0, sizeof(action));
	action.sa_handler = forward_signal;
	sigemptyset(&action.sa_mask);
	for (unsigned int i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		if (sigaction(signals[i], &action, &old_actions[i]) < 0)
			goto failure;
		installed++;
	}
	/* A child with its own process group needs foreground terminal access. */
	action.sa_handler = SIG_IGN;
	if (sigaction(SIGTTOU, &action, &old_ttou) < 0)
		goto failure;
	ttou_installed = 1;
	tty_fd = open("/dev/tty", O_RDWR);
	/* Some sandboxes deny /dev/tty while permitting inherited tty handles. */
	if (tty_fd < 0) {
		for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++) {
			if (isatty(fd) && tcgetpgrp(fd) >= 0) {
				tty_fd = dup(fd);
				if (tty_fd < 0)
					goto failure;
				break;
			}
		}
	}
	if (tty_fd >= 0) {
		if (fcntl(tty_fd, F_SETFD, FD_CLOEXEC) < 0)
			goto failure;
		foreground = tcgetpgrp(tty_fd) == getpgrp();
	}
	pid = fork();
	if (pid < 0)
		goto failure;
	if (pid == 0) {
		close(gate[1]);
		close(errors[0]);
		if (setpgid(0, 0) < 0)
			child_error(errors[1], errno);
		action.sa_handler = SIG_DFL;
		for (unsigned int i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
			if (sigaction(signals[i], &action, NULL) < 0)
				child_error(errors[1], errno);
		}
		if (sigaction(SIGPIPE, &action, NULL) < 0 ||
		    sigaction(SIGTTOU, &old_ttou, NULL) < 0)
			child_error(errors[1], errno);
		if (child_stdout_fd >= 0 && child_stdout_fd != STDOUT_FILENO) {
			if (dup2(child_stdout_fd, STDOUT_FILENO) < 0)
				child_error(errors[1], errno);
			close(child_stdout_fd);
		}
		do {
			n = read(gate[0], &token, sizeof(token));
		} while (n < 0 && errno == EINTR);
		close(gate[0]);
		if (n != 1)
			_exit(125);
		if (sigprocmask(SIG_SETMASK, &old_mask, NULL) < 0)
			child_error(errors[1], errno);
		execvp(argv[0], argv);
		child_error(errors[1], errno);
	}
	close(gate[0]);
	gate[0] = -1;
	close(errors[1]);
	errors[1] = -1;
	if (setpgid(pid, pid) < 0 && errno != EACCES)
		goto failure;
	command_pid = (sig_atomic_t)pid;
	child_group = pid;
	if (foreground && tcsetpgrp(tty_fd, pid) < 0)
		goto failure;
	if (start_measurement(context) < 0) {
		errno = EIO;
		goto failure;
	}
	do {
		n = write(gate[1], &token, sizeof(token));
	} while (n < 0 && errno == EINTR);
	if (n != 1)
		goto failure;
	close(gate[1]);
	gate[1] = -1;
	for (;;) {
		pid_t waited;

		if (resume_requested) {
			resume_requested = 0;
			if (tty_fd >= 0 && tcgetpgrp(tty_fd) == getpgrp() &&
			    tcsetpgrp(tty_fd, pid) < 0)
				goto failure;
			kill(-pid, SIGCONT);
		}
		waited = waitpid(pid, &status, WUNTRACED | WNOHANG);
		if (waited == 0) {
			/* Atomic unmask-and-wait prevents a lost exit/resume signal. */
			sigsuspend(&wait_mask);
			continue;
		}

		if (waited < 0) {
			if (errno == EINTR)
				continue;
			goto failure;
		}
		if (!WIFSTOPPED(status))
			break;
		/* Cooperate with shell job control for Ctrl-Z and background reads. */
		if (tty_fd >= 0 && tcgetpgrp(tty_fd) == pid)
			tcsetpgrp(tty_fd, getpgrp());
		kill(getpid(), SIGSTOP);
		foreground = tty_fd >= 0 && tcgetpgrp(tty_fd) == getpgrp();
		if (foreground && tcsetpgrp(tty_fd, pid) < 0)
			goto failure;
		kill(-pid, SIGCONT);
	}
	result = WIFEXITED(status) ? WEXITSTATUS(status) :
		  (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1);
	do {
		n = read(errors[0], &exec_error, sizeof(exec_error));
	} while (n < 0 && errno == EINTR);
	if (n == sizeof(exec_error))
		fprintf(stderr, "Error: cannot execute %s: %s\n",
			argv[0], strerror(exec_error));
	goto out;

failure:
	saved_errno = errno;
	if (pid > 0) {
		/* Also reaches the child if process-group setup failed. */
		kill(-pid, SIGKILL);
		kill(pid, SIGKILL);
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	}
	fprintf(stderr, "Error: command monitoring failed: %s\n",
		strerror(saved_errno));
out:
	if (mask_saved)
		sigprocmask(SIG_BLOCK, &blocked, NULL);
	command_pid = 0;
	if (tty_fd >= 0 && child_group > 0 &&
	    tcgetpgrp(tty_fd) == child_group &&
	    tcsetpgrp(tty_fd, getpgrp()) < 0) {
		fprintf(stderr, "Error: failed to restore terminal: %s\n",
			strerror(errno));
		result = -1;
	}
	if (tty_fd >= 0)
		close(tty_fd);
	for (int i = 0; i < installed; i++)
		sigaction(signals[i], &old_actions[i], NULL);
	if (ttou_installed)
		sigaction(SIGTTOU, &old_ttou, NULL);
	if (mask_saved)
		sigprocmask(SIG_SETMASK, &old_mask, NULL);
	for (int i = 0; i < 2; i++) {
		if (gate[i] >= 0)
			close(gate[i]);
		if (errors[i] >= 0)
			close(errors[i]);
	}
	return result;
}
