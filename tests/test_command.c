/* SPDX-License-Identifier: GPL-2.0 */
/* Exercise real child processes without requiring Linux telemetry. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "command.h"

/* Keep real pipes, but fail each CLOEXEC operation before any child starts. */
static int fault_fds[4];
static int fault_close_calls[4];
static int fault_fd_count;
static int fault_pipe_calls;
static int fault_fcntl_calls;
static int fault_fcntl_fail_at;
static int fault_fork_calls;

static int fault_pipe(int fds[2])
{
	int result = pipe(fds);

	assert(result == 0);
	assert(fault_fd_count + 2 <= 4);
	fault_pipe_calls++;
	fault_fds[fault_fd_count++] = fds[0];
	fault_fds[fault_fd_count++] = fds[1];
	return result;
}

static int fault_fcntl(int fd, int operation, int value)
{
	assert(operation == F_SETFD && value == FD_CLOEXEC);
	if (++fault_fcntl_calls == fault_fcntl_fail_at) {
		errno = EIO;
		return -1;
	}
	return fcntl(fd, operation, value);
}

static int fault_close(int fd)
{
	int result;
	int index;

	for (index = 0; index < fault_fd_count; index++) {
		if (fault_fds[index] == fd)
			break;
	}
	assert(index < fault_fd_count);
	fault_close_calls[index]++;
	result = close(fd);
	/* A successful close need not preserve errno: retain the fcntl error. */
	errno = ENOSPC;
	return result;
}

static pid_t fault_fork(void)
{
	fault_fork_calls++;
	errno = EAGAIN;
	return -1;
}

static int fault_run_monitored_command(char *const argv[], int child_stdout_fd,
				       int (*start_measurement)(void *),
				       void *context);

#define pipe fault_pipe
#define fcntl fault_fcntl
#define close fault_close
#define fork fault_fork
#define run_monitored_command fault_run_monitored_command
#include "../src/app/command.c"
#undef run_monitored_command
#undef fork
#undef close
#undef fcntl
#undef pipe

static const char *self_path;
static int tree_signal_fd;

static int begin_ok(void *context)
{
	(void)context;
	return 0;
}

static int begin_fail(void *context)
{
	(void)context;
	return -1;
}

static int begin_interrupt(void *context)
{
	(void)context;
	assert(kill(getpid(), SIGINT) == 0);
	return 0;
}

static int count_measurement(void *context)
{
	int *calls = context;

	(*calls)++;
	return 0;
}

static void test_pipe_cloexec_failure_cleanup(void)
{
	char *argv[] = { "/bin/sh", "-c", "exit 0", NULL };
	int failure;

	/* gate read/write, followed by errors read/write. */
	for (failure = 1; failure <= 4; failure++) {
		FILE *diagnostic = tmpfile();
		char output[256] = {0};
		int saved_stderr;
		int measurement_calls = 0;
		int index;

		assert(diagnostic);
		assert(fflush(stderr) == 0);
		saved_stderr = dup(STDERR_FILENO);
		assert(saved_stderr >= 0);
		assert(dup2(fileno(diagnostic), STDERR_FILENO) >= 0);
		memset(fault_close_calls, 0, sizeof(fault_close_calls));
		fault_fd_count = 0;
		fault_pipe_calls = 0;
		fault_fcntl_calls = 0;
		fault_fcntl_fail_at = failure;
		fault_fork_calls = 0;

		assert(fault_run_monitored_command(argv, -1, count_measurement,
						   &measurement_calls) == -1);
		assert(fflush(stderr) == 0);
		assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
		close(saved_stderr);
		rewind(diagnostic);
		assert(fread(output, 1, sizeof(output) - 1, diagnostic) > 0);
		fclose(diagnostic);

		/* Error reporting must retain the original fcntl errno after close. */
		assert(strstr(output, strerror(EIO)));
		assert(!strstr(output, strerror(ENOSPC)));
		assert(fault_fcntl_calls == failure);
		assert(fault_pipe_calls == (failure + 1) / 2);
		assert(fault_fd_count == 2 * fault_pipe_calls);
		assert(fault_fork_calls == 0 && measurement_calls == 0);
		for (index = 0; index < fault_fd_count; index++) {
			assert(fault_close_calls[index] == 1);
			errno = 0;
			assert(fcntl(fault_fds[index], F_GETFD) < 0 && errno == EBADF);
		}
	}
}

static void test_exit_and_stdout(void)
{
	FILE *capture = tmpfile();
	char output[32] = {0};
	char *argv[] = { "/bin/sh", "-c", "printf child-output; exit 7", NULL };

	assert(capture);
	assert(run_monitored_command(argv, fileno(capture), begin_ok, NULL) == 7);
	rewind(capture);
	assert(fread(output, 1, sizeof(output) - 1, capture) == 12);
	assert(!strcmp(output, "child-output"));
	fclose(capture);
}

static void test_exec_failures(void)
{
	char path[] = "/tmp/armstat-noexec.XXXXXX";
	int fd = mkstemp(path);
	char *missing[] = { "/nonexistent-armstat-command-for-test", NULL };
	char *noexec[] = { path, NULL };

	assert(fd >= 0);
	close(fd);
	assert(chmod(path, 0600) == 0);
	assert(run_monitored_command(missing, -1, begin_ok, NULL) == 127);
	assert(run_monitored_command(noexec, -1, begin_ok, NULL) == 126);
	unlink(path);
}

static void test_failed_baseline_does_not_execute(void)
{
	char path[] = "/tmp/armstat-command-side-effect.XXXXXX";
	int fd = mkstemp(path);
	char *argv[] = { "/bin/sh", "-c", "printf executed > \"$1\"",
			"sh", path, NULL };

	assert(fd >= 0);
	close(fd);
	unlink(path);
	assert(run_monitored_command(argv, -1, begin_fail, NULL) == -1);
	assert(access(path, F_OK) < 0 && errno == ENOENT);
}

static void test_signal_status_and_gate_interrupt(void)
{
	char *pipe_signal[] = { "/bin/sh", "-c", "kill -PIPE $$; exit 5", NULL };
	char *sleep_command[] = { "/bin/sleep", "30", NULL };

	/* armstat ignores SIGPIPE, but the executed command must not inherit it. */
	assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
	assert(run_monitored_command(pipe_signal, -1, begin_ok, NULL) == 141);
	assert(run_monitored_command(sleep_command, -1, begin_interrupt, NULL) == 130);
}

static void test_descriptor_ownership(void)
{
	int private_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	int user_fd = open("/dev/null", O_RDONLY);
	char private_arg[32], user_arg[32];
	char *argv[] = { (char *)self_path, "--check-fds",
			private_arg, user_arg, NULL };

	assert(private_fd >= 0 && user_fd >= 0);
	snprintf(private_arg, sizeof(private_arg), "%d", private_fd);
	snprintf(user_arg, sizeof(user_arg), "%d", user_fd);
	assert(run_monitored_command(argv, -1, begin_ok, NULL) == 0);
	/* The monitor still owns both originals after the child's exec. */
	assert(fcntl(private_fd, F_GETFD) >= 0 && fcntl(user_fd, F_GETFD) >= 0);
	close(private_fd);
	close(user_fd);
}

struct notifier_context {
	int ready_fd;
	pid_t notifier;
};

static int begin_notifier(void *data)
{
	struct notifier_context *context = data;
	pid_t monitor = getpid();

	context->notifier = fork();
	assert(context->notifier >= 0);
	if (!context->notifier) {
		char ready;

		alarm(5);
		if (read(context->ready_fd, &ready, 1) != 1 || ready != 'R')
			_exit(2);
		_exit(kill(monitor, SIGTERM) == 0 ? 0 : 3);
	}
	return 0;
}

static void test_signal_forwarding_reaches_descendants(void)
{
	int ready[2], terminated[2], status;
	char ready_arg[32], terminated_arg[32], byte;
	struct notifier_context context;
	char *argv[] = { (char *)self_path, "--signal-tree",
			ready_arg, terminated_arg, NULL };

	assert(pipe(ready) == 0 && pipe(terminated) == 0);
	snprintf(ready_arg, sizeof(ready_arg), "%d", ready[1]);
	snprintf(terminated_arg, sizeof(terminated_arg), "%d", terminated[1]);
	context.ready_fd = ready[0];
	context.notifier = -1;
	assert(run_monitored_command(argv, -1, begin_notifier, &context) == 143);
	assert(waitpid(context.notifier, &status, 0) == context.notifier);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(read(terminated[0], &byte, 1) == 1 && byte == 'T');
	close(ready[0]);
	close(ready[1]);
	close(terminated[0]);
	close(terminated[1]);
}

static void leaf_terminated(int sig)
{
	char byte = 'T';
	ssize_t written;

	(void)sig;
	written = write(tree_signal_fd, &byte, 1);
	_exit(written == 1 ? 0 : 2);
}

static int signal_tree(int ready_fd, int terminated_fd)
{
	int leaf_ready[2];
	pid_t leaf;
	char ready = 'R';

	if (pipe(leaf_ready) < 0)
		return 2;
	leaf = fork();
	if (leaf < 0)
		return 2;
	if (!leaf) {
		struct sigaction action;

		memset(&action, 0, sizeof(action));
		action.sa_handler = leaf_terminated;
		sigemptyset(&action.sa_mask);
		tree_signal_fd = terminated_fd;
		if (sigaction(SIGTERM, &action, NULL) < 0 ||
		    write(leaf_ready[1], &ready, 1) != 1)
			return 2;
		for (;;)
			pause();
	}
	if (read(leaf_ready[0], &ready, 1) != 1 ||
	    write(ready_fd, &ready, 1) != 1)
		return 2;
	for (;;)
		pause();
}

static void run_case(void (*test)(void))
{
	pid_t pid = fork();
	int status;

	assert(pid >= 0);
	if (!pid) {
		alarm(8);
		test();
		_exit(0);
	}
	assert(waitpid(pid, &status, 0) == pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv)
{
	self_path = argv[0];
	if (argc == 3 && !strcmp(argv[1], "--terminal-gate")) {
		char byte, input[64];

		puts("READY");
		fflush(stdout);
		if (read(atoi(argv[2]), &byte, 1) != 1 ||
		    !fgets(input, sizeof(input), stdin))
			return 2;
		printf("RESULT:%s", input);
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "--check-fds")) {
		int private_fd = atoi(argv[2]);
		int user_fd = atoi(argv[3]);

		errno = 0;
		return !(fcntl(private_fd, F_GETFD) < 0 && errno == EBADF &&
			 fcntl(user_fd, F_GETFD) >= 0);
	}
	if (argc == 4 && !strcmp(argv[1], "--signal-tree"))
		return signal_tree(atoi(argv[2]), atoi(argv[3]));
	if (argc >= 3 && !strcmp(argv[1], "--run")) {
		int child_stdout = dup(STDOUT_FILENO);
		int result;

		assert(child_stdout >= 0);
		assert(fcntl(child_stdout, F_SETFD, FD_CLOEXEC) == 0);
		assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);
		result = run_monitored_command(argv + 2, child_stdout, begin_ok, NULL);
		close(child_stdout);
		fprintf(stderr, "COMMAND_DONE status=%d foreground=%d group=%d\n",
			result, (int)tcgetpgrp(STDIN_FILENO), (int)getpgrp());
		return result < 0 ? 1 : result;
	}
	run_case(test_pipe_cloexec_failure_cleanup);
	run_case(test_exit_and_stdout);
	run_case(test_exec_failures);
	run_case(test_failed_baseline_does_not_execute);
	run_case(test_signal_status_and_gate_interrupt);
	run_case(test_descriptor_ownership);
	run_case(test_signal_forwarding_reaches_descendants);
	puts("command lifecycle tests: PASS (7 cases)");
	return 0;
}
