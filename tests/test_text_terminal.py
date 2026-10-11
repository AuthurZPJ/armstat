#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise text headers through real PTYs, without requiring ARM telemetry."""

import csv
import errno
import fcntl
import io
import json
import math
import os
from pathlib import Path
import pty
import re
import selectors
import struct
import subprocess
import termios
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch


TEST_BIN = os.environ.get(
    "ARMSTAT_TEST_BIN", str(Path(__file__).with_name("test_runtime_smoke"))
)
TIMEOUT = 5.0
SAMPLE_END = b"\x1earmstat-fixture-end\n"
CPU_FIELDS = ["-s", "freq_mhz,cpu_busy_percent,cpu_idle_percent"]
SUMMARY_FIELDS = ["-s", "summary_freq_mhz"]
MIXED_FIELDS = ["-s", "summary_freq_mhz,pkg_freq_mhz,freq_mhz"]


class Fixture:
    """Frame samples on stdout, with commands and diagnostics on separate pipes."""

    def __init__(self, count, args, size=None):
        self.selector = selectors.DefaultSelector()
        self.output = bytearray()
        self.stderr = bytearray()
        self.diagnostics = []
        self.ready = 0
        self.finished = False
        self.master = None
        slave = None
        try:
            if size is not None:
                self.master, slave = pty.openpty()
                attrs = termios.tcgetattr(slave)
                attrs[1] &= ~termios.ONLCR
                termios.tcsetattr(slave, termios.TCSANOW, attrs)
                self.resize(*size)
            self.proc = subprocess.Popen(
                [TEST_BIN, "--text-fixture", str(count), *args],
                stdin=subprocess.PIPE,
                stdout=slave if slave is not None else subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
            )
            stdout_fd = self.master if slave is not None else self.proc.stdout.fileno()
            for fd, name in [(stdout_fd, "stdout"), (self.proc.stderr.fileno(), "stderr")]:
                os.set_blocking(fd, False)
                self.selector.register(fd, selectors.EVENT_READ, name)
        except BaseException:
            if hasattr(self, "proc") and self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait(timeout=TIMEOUT)
            self._close_fds()
            raise
        finally:
            if slave is not None:
                os.close(slave)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        try:
            if exc_type is None:
                self.finish()
        finally:
            if self.proc.poll() is None:
                self.proc.kill()
                self.proc.wait(timeout=TIMEOUT)
            self._close_fds()

    def _close_fds(self):
        self.selector.close()
        if hasattr(self, "proc"):
            for stream in [self.proc.stdin, self.proc.stdout, self.proc.stderr]:
                if stream is not None:
                    stream.close()
        if self.master is not None:
            os.close(self.master)
            self.master = None

    def resize(self, rows, columns):
        assert self.master is not None
        fcntl.ioctl(
            self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0)
        )

    def _pump(self, timeout):
        events = self.selector.select(timeout)
        for key, _ in events:
            try:
                data = os.read(key.fd, 65536)
            except BlockingIOError:
                continue
            except OSError as error:
                if error.errno == errno.EIO and key.data == "stdout":
                    data = b""  # A PTY reports EIO after its final slave closes.
                else:
                    raise
            if not data:
                self.selector.unregister(key.fd)
            elif key.data == "stdout":
                self.output.extend(data)
            else:
                self.stderr.extend(data)
                while b"\n" in self.stderr:
                    line, _, rest = self.stderr.partition(b"\n")
                    self.stderr = bytearray(rest)
                    if line == b"ready":
                        self.ready += 1
                    else:
                        self.diagnostics.append(line.decode("utf-8", errors="replace"))
        return bool(events)

    def _take_output(self):
        output = self.output.decode("utf-8")
        self.output.clear()
        return output

    def emit(self):
        self.proc.stdin.write(b"emit\n")
        self.proc.stdin.flush()
        deadline = time.monotonic() + TIMEOUT
        while True:
            end = self.output.find(SAMPLE_END)
            if end >= 0:
                output = self.output[:end].decode("utf-8")
                del self.output[:end + len(SAMPLE_END)]
                return output
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"fixture emit timed out: {self.diagnostics}")
            self._pump(min(remaining, 0.1))
            if self.proc.poll() is not None and SAMPLE_END not in self.output:
                raise AssertionError(
                    f"fixture exited {self.proc.returncode}: {self.diagnostics}"
                )

    def finish(self):
        if self.finished:
            return ""
        self.proc.stdin.write(b"quit\n")
        self.proc.stdin.close()
        deadline = time.monotonic() + TIMEOUT
        while self.selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"fixture exit timed out: {self.diagnostics}")
            self._pump(min(remaining, 0.1))
        result = self.proc.wait(timeout=max(0.01, deadline - time.monotonic()))
        if result != 0:
            raise AssertionError(f"fixture exited {result}: {self.diagnostics}")
        self.finished = True
        return self._take_output()


def cpu_ids(output):
    return [int(match[1]) for match in re.finditer(r"(?m)^(\d+)\s", output)]


def cpu_headers(output):
    return [line for line in output.splitlines() if re.match(r"^CPU(?:\s|$)", line)]


def summary_headers(output):
    return sum(line.startswith("Timestamp") for line in output.splitlines())


class FixtureProtocolTests(unittest.TestCase):
    def make_fixture(self):
        fixture = Fixture.__new__(Fixture)
        fixture.output = bytearray()
        fixture.stderr = bytearray()
        fixture.diagnostics = []
        fixture.ready = 0
        fixture.proc = SimpleNamespace(stdin=io.BytesIO(), poll=lambda: None)
        return fixture

    def test_emit_waits_for_stdout_frame_after_early_stderr_ready(self):
        fixture = self.make_fixture()
        payload = "CPU Freq\n0 2200\n".encode()
        steps = [
            ("stderr", b"ready\n"),
            None,  # PTY data can be temporarily absent after the pipe ack.
            ("stdout", payload[:5]),
            ("stdout", payload[5:] + SAMPLE_END[:1]),
            ("stdout", SAMPLE_END[1:8]),
            ("stdout", SAMPLE_END[8:]),
        ]
        read_data = []

        def select(timeout):
            self.assertTrue(steps, "emit read beyond its completed frame")
            step = steps.pop(0)
            if step is None:
                return []
            name, data = step
            read_data.append(data)
            return [(SimpleNamespace(fd=101, data=name), selectors.EVENT_READ)]

        fixture.selector = SimpleNamespace(select=select)
        with patch.object(os, "read", side_effect=lambda fd, size: read_data.pop(0)):
            self.assertEqual(fixture.emit(), payload.decode())
        self.assertEqual(fixture.ready, 1)
        self.assertEqual(fixture.proc.stdin.getvalue(), b"emit\n")
        self.assertEqual(fixture.output, b"")
        self.assertEqual(steps, [])
        self.assertEqual(read_data, [])

    def test_emit_removes_only_one_frame_and_preserves_following_bytes(self):
        fixture = self.make_fixture()
        fixture.output.extend(b"first\n" + SAMPLE_END + b"second\n" + SAMPLE_END)
        fixture._pump = lambda timeout: self.fail("complete frames need no reads")
        self.assertEqual(fixture.emit(), "first\n")
        self.assertEqual(fixture.output, b"second\n" + SAMPLE_END)
        self.assertEqual(fixture.emit(), "second\n")
        self.assertEqual(fixture.output, b"")
        self.assertEqual(fixture.proc.stdin.getvalue(), b"emit\nemit\n")


class TextTerminalTests(unittest.TestCase):
    def assert_cpu_sample(self, output, count=128):
        self.assertEqual(cpu_ids(output), list(range(count)))
        self.assertEqual(output.count("--- interval"), 1)

    def test_large_cpu_table_repeats_after_twenty_one_rows(self):
        with Fixture(128, CPU_FIELDS, (24, 120)) as fixture:
            output = fixture.emit()
            self.assert_cpu_sample(output)
            self.assertEqual(len(cpu_headers(output)), 7)
            lines = output.splitlines()
            headers = [i for i, line in enumerate(lines) if line.startswith("CPU")]
            self.assertEqual(cpu_ids("\n".join(lines[headers[0] + 1:headers[1]])),
                             list(range(21)))
            second = fixture.emit()
            self.assert_cpu_sample(second)
            self.assertEqual(len(cpu_headers(second)), 7)

    def test_mixed_scopes_have_their_own_headers_and_one_marker(self):
        with Fixture(128, MIXED_FIELDS, (24, 120)) as fixture:
            for _ in range(2):
                output = fixture.emit()
                self.assert_cpu_sample(output)
                self.assertEqual(len(cpu_headers(output)), 7)
                self.assertEqual(len(re.findall(r"(?m)^Pkg\s", output)), 7)
                self.assertEqual(len(re.findall(r"(?m)^Pkg\d+\s", output)), 128)
                self.assertEqual(len(re.findall(r"(?m)^SUM\s+Freq", output)), 1)
                self.assertEqual(len(re.findall(r"(?m)^SUM\s+2200", output)), 1)

    def test_cpu_filter_uses_only_tracked_fixture_rows(self):
        for selected in [0, 1]:
            for size in [None, (24, 120)]:
                args = ["-P", "-a", "-c", str(selected)]
                with self.subTest(cpu=selected, size=size), Fixture(2, args, size) as fixture:
                    for _ in range(2):
                        output = fixture.emit()
                        # Include negative IDs so a phantom -1 row cannot escape the check.
                        self.assertEqual(re.findall(r"(?m)^(-?\d+)\s", output),
                                         [str(selected)])
                        self.assertEqual(len(re.findall(r"(?m)^SUM\s+2200", output)), 1)
                        self.assertEqual(output.count("--- interval"), 1)
                        self.assertNotRegex(output, r"(?m)^Pkg\d+\s")

    def test_compact_summary_tracks_height_across_samples(self):
        with Fixture(1, SUMMARY_FIELDS, (6, 120)) as fixture:
            samples = [fixture.emit() for _ in range(6)]
        self.assertEqual([summary_headers(sample) for sample in samples],
                         [1, 0, 0, 1, 0, 0])
        self.assertNotIn("--- interval", "".join(samples))
        self.assertEqual(sum(len(sample.splitlines()) for sample in samples), 8)

    def test_resize_reloads_both_height_and_width(self):
        with Fixture(128, CPU_FIELDS, (24, 120)) as fixture:
            self.assertEqual(len(cpu_headers(fixture.emit())), 7)
            fixture.resize(12, 120)
            shorter = fixture.emit()
            self.assert_cpu_sample(shorter)
            self.assertEqual(len(cpu_headers(shorter)), math.ceil(128 / 9))
            fixture.resize(24, 10)
            narrower = fixture.emit()
            self.assert_cpu_sample(narrower)
            header = cpu_headers(narrower)[0]
            wrapped_lines = math.ceil(len(header) / 10)
            rows_per_header = 22 // wrapped_lines - 1
            self.assertEqual(len(cpu_headers(narrower)), math.ceil(128 / rows_per_header))
            self.assertGreater(len(cpu_headers(narrower)), 7)
            fixture.resize(24, 120)
            self.assertEqual(len(cpu_headers(fixture.emit())), 7)
        with Fixture(1, SUMMARY_FIELDS, (24, 120)) as fixture:
            self.assertEqual(summary_headers(fixture.emit()), 1)
            self.assertEqual(summary_headers(fixture.emit()), 0)
            fixture.resize(24, 80)
            self.assertEqual(summary_headers(fixture.emit()), 1)
            self.assertEqual(summary_headers(fixture.emit()), 0)
            fixture.resize(12, 80)
            self.assertEqual(summary_headers(fixture.emit()), 1)

    def test_zero_geometry_falls_back_to_nonautomatic_headers(self):
        for size in [(0, 120), (24, 0), (0, 0)]:
            with self.subTest(size=size), Fixture(128, CPU_FIELDS, size) as fixture:
                first, second = fixture.emit(), fixture.emit()
                self.assert_cpu_sample(first)
                self.assert_cpu_sample(second)
                self.assertEqual(len(cpu_headers(first)), 1)
                self.assertEqual(len(cpu_headers(second)), 0)

    def test_tiny_terminal_does_not_repeat_a_header_for_each_row(self):
        for size in [(1, 120), (3, 120), (24, 1)]:
            with self.subTest(size=size), Fixture(128, CPU_FIELDS, size) as fixture:
                output = fixture.emit()
                self.assert_cpu_sample(output)
                self.assertEqual(len(cpu_headers(output)), 1)
            with self.subTest(summary_size=size), Fixture(1, SUMMARY_FIELDS, size) as fixture:
                samples = [fixture.emit() for _ in range(4)]
                self.assertEqual([summary_headers(sample) for sample in samples],
                                 [1, 0, 0, 0])

    def test_explicit_header_intervals_override_terminal_pagination(self):
        choices = [(["-N", "0"], [1, 0, 0]),
                   (["-N", "1"], [1, 1, 1]),
                   (["-N", "2"], [1, 0, 1]),
                   (["-N", "1", "-N", "0"], [1, 0, 0]),
                   (["-N", "0", "-N", "2"], [1, 0, 1])]
        for args, expected in choices:
            with self.subTest(args=args), Fixture(128, CPU_FIELDS + args, (24, 120)) as fixture:
                samples = [fixture.emit() for _ in range(3)]
                self.assertEqual([len(cpu_headers(sample)) for sample in samples], expected)
                for sample in samples:
                    self.assert_cpu_sample(sample)
        with Fixture(1, SUMMARY_FIELDS + ["-N", "2"], (6, 10)) as fixture:
            self.assertEqual([summary_headers(fixture.emit()) for _ in range(3)],
                             [1, 0, 1])

    def test_raw_pmu_columns_contribute_to_wrapped_summary_height(self):
        args = ["-S", "-s", "pmu", "-p", "cycles,instructions"]
        with Fixture(1, args, (24, 20)) as fixture:
            samples = [fixture.emit() for _ in range(10)]
        header = samples[0].splitlines()[0]
        self.assertIn("cycles", header)
        self.assertIn("instructions", header)
        wrapped_lines = math.ceil(len(header) / 20)
        self.assertGreater(wrapped_lines, 1)
        rows_per_header = 22 // wrapped_lines - 1
        self.assertGreater(rows_per_header, 0)
        self.assertEqual([summary_headers(sample) for sample in samples],
                         [int(i % rows_per_header == 0) for i in range(10)])
        self.assertNotIn("--- interval", "".join(samples))

    def test_quiet_preserves_terminal_headers_timestamps_and_markers(self):
        with Fixture(128, CPU_FIELDS + ["-q"], (24, 120)) as fixture:
            for _ in range(2):
                output = fixture.emit()
                self.assert_cpu_sample(output)
                self.assertEqual(len(cpu_headers(output)), 7)
                self.assertRegex(output, r"\.\d{3}\+00:00")
        with Fixture(1, SUMMARY_FIELDS + ["-q"], (6, 120)) as fixture:
            first, second = fixture.emit(), fixture.emit()
            self.assertEqual(summary_headers(first), 1)
            self.assertEqual(summary_headers(second), 0)
            for output in [first, second]:
                self.assertRegex(output, r"\.\d{3}\+00:00")
                self.assertNotIn("--- interval", output)

    def test_quiet_all_has_no_headers_timestamps_or_markers(self):
        with Fixture(128, CPU_FIELDS + ["--quiet-all"], (6, 10)) as fixture:
            for _ in range(2):
                output = fixture.emit()
                self.assertEqual(cpu_ids(output), list(range(128)))
                self.assertEqual(cpu_headers(output), [])
                self.assertNotIn("--- interval", output)
                self.assertNotRegex(output, r"\.\d{3}\+00:00")
        with Fixture(1, SUMMARY_FIELDS + ["--quiet-all"], (6, 10)) as fixture:
            for _ in range(2):
                output = fixture.emit()
                self.assertEqual(summary_headers(output), 0)
                self.assertTrue(output.startswith("SUM"))
                self.assertEqual(len(output.splitlines()), 1)

    def test_pipe_preserves_rows_without_automatic_repetition(self):
        with Fixture(128, CPU_FIELDS) as fixture:
            first, second = fixture.emit(), fixture.emit()
            self.assertEqual(len(cpu_headers(first)), 1)
            self.assertEqual(len(cpu_headers(second)), 0)
            self.assert_cpu_sample(first)
            self.assert_cpu_sample(second)

    def test_machine_formats_are_unchanged_by_a_terminal(self):
        for fmt in ["json", "csv"]:
            results = []
            for size in [None, (6, 10)]:
                with Fixture(4, MIXED_FIELDS + ["-f", fmt], size) as fixture:
                    output = fixture.emit() + fixture.emit() + fixture.finish()
                if fmt == "json":
                    records = json.loads(output)
                    self.assertEqual(len(records), 2)
                    self.assertTrue(all(len(record["cpus"]) == 4 for record in records))
                    results.append(records)
                else:
                    rows = list(csv.reader(io.StringIO(output)))
                    self.assertEqual(len({len(row) for row in rows}), 1)
                    self.assertEqual(len(rows), 19)  # header + 2 * (SUM + 4 PKG + 4 CPU)
                    results.append(rows)
            self.assertEqual(results[0], results[1], fmt)


if __name__ == "__main__":
    unittest.main()
