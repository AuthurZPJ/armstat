#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Real controlling-terminal checks for the telemetry-independent runner."""
import errno
import fcntl
import os
import pty
import select
import signal
import termios
import time
import unittest

BIN = os.path.abspath(os.environ.get("ARMSTAT_COMMAND_TEST_BIN", "tests/test_command"))


def read_until(fd, expected, timeout=8):
    data = b""
    deadline = time.monotonic() + timeout
    while expected not in data:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise AssertionError(f"timed out waiting for {expected!r}: {data!r}")
        try:
            chunk = os.read(fd, 4096)
        except OSError as error:
            if error.errno != errno.EIO:
                raise
            chunk = b""
        if not chunk:
            raise AssertionError(f"closed before {expected!r}: {data!r}")
        data += chunk
    return data


def reap(pid, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        waited, status = os.waitpid(pid, os.WNOHANG)
        if waited:
            return os.waitstatus_to_exitcode(status)
        time.sleep(0.01)
    raise AssertionError(f"process {pid} did not finish")


class TerminalCommand(unittest.TestCase):
    def start(self, command):
        master, slave = pty.openpty()
        pid = os.fork()
        if not pid:
            os.close(master)
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            for target in (0, 1, 2):
                os.dup2(slave, target)
            if slave > 2:
                os.close(slave)
            os.execv(BIN, [BIN, "--run", "/bin/sh", "-c", command])
        os.close(slave)
        self.addCleanup(os.close, master)

        def cleanup():
            try:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            except ProcessLookupError:
                pass
            except ChildProcessError:
                pass

        self.addCleanup(cleanup)
        return master, pid

    def test_foreground_stdin_and_terminal_restore(self):
        master, pid = self.start('printf READY; read value; printf "RESULT:%s\\n" "$value"')
        read_until(master, b"READY")
        self.assertNotEqual(os.tcgetpgrp(master), pid)
        os.write(master, b"hello\n")
        output = read_until(master, f"foreground={pid} group={pid}".encode())
        self.assertIn(b"RESULT:hello", output)
        self.assertIn(f"foreground={pid} group={pid}".encode(), output)
        self.assertEqual(reap(pid), 0)

    def test_terminal_ctrl_c_preserves_status(self):
        master, pid = self.start("printf READY; exec sleep 30")
        read_until(master, b"READY")
        os.write(master, b"\x03")
        read_until(master, b"status=130")
        self.assertEqual(reap(pid), 130)

    def test_parent_term_reaches_foreground_child(self):
        master, pid = self.start("printf READY; exec sleep 30")
        read_until(master, b"READY")
        os.kill(pid, signal.SIGTERM)
        read_until(master, b"status=143")
        self.assertEqual(reap(pid), 143)

    def test_stop_background_and_foreground_resume(self):
        self.exercise_job_control(background_read=True)

    def test_foreground_resume_while_child_is_running(self):
        self.exercise_job_control(background_read=False)

    def exercise_job_control(self, background_read):
        master, slave = pty.openpty()
        controls = os.pipe()
        reports = os.pipe()
        gate = os.pipe()
        supervisor = os.fork()
        if not supervisor:
            os.close(master)
            os.close(controls[1])
            os.close(reports[0])
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            signal.signal(signal.SIGTTOU, signal.SIG_IGN)
            monitor = -1
            try:
                monitor = os.fork()
                if not monitor:
                    os.setpgid(0, 0)
                    for target in (0, 1, 2):
                        os.dup2(slave, target)
                    if slave > 2:
                        os.close(slave)
                    if background_read:
                        command = 'printf READY; read value; printf "RESULT:%s\\n" "$value"'
                        os.execv(BIN, [BIN, "--run", "/bin/sh", "-c", command])
                    os.set_inheritable(gate[0], True)
                    os.execv(BIN, [BIN, "--run", BIN, "--terminal-gate", str(gate[0])])
                os.close(gate[0])
                os.setpgid(monitor, monitor)
                os.tcsetpgrp(slave, monitor)
                # The fake shell waits for the monitor to mirror the child's stop.
                waited, status = os.waitpid(monitor, os.WUNTRACED)
                assert waited == monitor and os.WIFSTOPPED(status)
                os.tcsetpgrp(slave, os.getpgrp())
                os.write(reports[1], b"STOPPED\n")
                assert os.read(controls[0], 1) == b"b"
                os.kill(monitor, signal.SIGCONT)
                if background_read:
                    waited, status = os.waitpid(monitor, os.WUNTRACED)
                    assert waited == monitor and os.WIFSTOPPED(status)
                else:
                    time.sleep(0.1)
                    assert os.waitpid(monitor, os.WNOHANG | os.WUNTRACED)[0] == 0
                # Neither a stopped read nor running background work steals the tty.
                assert os.tcgetpgrp(slave) == os.getpgrp()
                os.write(reports[1], b"BACKGROUND_OK\n")
                assert os.read(controls[0], 1) == b"f"
                os.tcsetpgrp(slave, monitor)
                os.kill(monitor, signal.SIGCONT)
                if not background_read:
                    deadline = time.monotonic() + 8
                    while os.tcgetpgrp(slave) == monitor:
                        assert time.monotonic() < deadline, "fg did not hand terminal to child"
                        time.sleep(0.01)
                    assert os.tcgetpgrp(slave) != os.getpgrp()
                    os.write(gate[1], b"r")
                os.write(reports[1], b"FOREGROUND\n")
                waited, status = os.waitpid(monitor, 0)
                assert waited == monitor and os.waitstatus_to_exitcode(status) == 0
                os.tcsetpgrp(slave, os.getpgrp())
                os.write(reports[1], b"DONE\n")
                os._exit(0)
            except BaseException as error:
                os.write(reports[1], ("ERROR:" + repr(error) + "\n").encode())
                if monitor > 0:
                    os.kill(monitor, signal.SIGKILL)
                os._exit(1)
        os.close(slave)
        os.close(controls[0])
        os.close(reports[1])
        os.close(gate[0])
        os.close(gate[1])

        def cleanup():
            for fd in (master, controls[1], reports[0]):
                os.close(fd)
            try:
                os.kill(supervisor, signal.SIGKILL)
                os.waitpid(supervisor, 0)
            except ProcessLookupError:
                pass
            except ChildProcessError:
                pass

        self.addCleanup(cleanup)
        read_until(master, b"READY")
        os.write(master, b"\x1a")
        read_until(reports[0], b"STOPPED")
        os.write(controls[1], b"b")
        read_until(reports[0], b"BACKGROUND_OK")
        os.write(controls[1], b"f")
        read_until(reports[0], b"FOREGROUND")
        os.write(master, b"hello\n")
        read_until(master, b"RESULT:hello")
        read_until(reports[0], b"DONE")
        self.assertEqual(reap(supervisor), 0)


if __name__ == "__main__":
    unittest.main()
