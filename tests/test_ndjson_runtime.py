#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""ARM64 Linux NDJSON sampling, command, live-reader and failure contracts."""

import json
import os
from pathlib import Path
import platform
import select
import signal
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from armstat_loader import load_cpu_series, load_summary_series  # noqa: E402


def main():
    if platform.system() != "Linux" or platform.machine() not in {"aarch64", "arm64"}:
        print("NDJSON runtime tests skipped: requires ARM64 Linux.")
        return
    binary = str(Path(os.environ.get("ARMSTAT_BIN", ROOT / "armstat")).resolve())

    def run(options):
        result = subprocess.run([binary, "-P", *options], capture_output=True,
                                timeout=15)
        assert result.returncode == 0, result.stderr
        return result

    with tempfile.TemporaryDirectory(prefix="armstat-ndjson-") as temporary:
        root = Path(temporary)
        for index, view in enumerate(([], ["-a"], ["-s", "all"], ["-s", "ctx_switches"])):
            path = root / f"view-{index}.ndjson"
            result = run([*view, "-f", "ndjson", "-o", str(path),
                          "-i", "0.01", "-n", "3"])
            assert result.stdout == b""
            raw = path.read_bytes()
            assert raw.endswith(b"\n")
            records = [json.loads(line) for line in raw.splitlines()]
            assert [item["interval"] for item in records] == [1, 2, 3]
            assert all(item["schema_version"] == 9 and item["duration_us"] > 0
                       for item in records)
            assert len(load_summary_series(path).rows) == 3
            if view in (["-a"], ["-s", "all"]):
                assert len(load_cpu_series(path).samples) == 3

        command_path = root / "command.jsonl"
        result = run(["-s", "ctx_switches", "-f", "ndjson", "-o", str(command_path),
                      "--", sys.executable, "-c", "print('child-stdout')"])
        assert result.stdout == b"child-stdout\n"
        record, = [json.loads(line) for line in command_path.read_bytes().splitlines()]
        assert record["interval"] == 1 and record["duration_us"] > 0

        # Read a complete sample before exit, then force termination without
        # allowing a serializer close step. Already complete lines survive.
        live = subprocess.Popen(
            [binary, "-P", "-s", "ctx_switches", "-f", "ndjson",
             "-i", "0.01", "-n", "0"], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            prefix = b""
            deadline = time.monotonic() + 10
            while b"\n" not in prefix:
                remaining = deadline - time.monotonic()
                assert remaining > 0, "no flushed NDJSON sample"
                assert select.select([live.stdout], [], [], remaining)[0]
                chunk = os.read(live.stdout.fileno(), 65536)
                assert chunk, "capture exited before a complete sample"
                prefix += chunk
            assert json.loads(prefix.split(b"\n", 1)[0])["interval"] == 1
            assert live.poll() is None, "sample was not available while capture ran"
            live.kill()
            remainder, _ = live.communicate(timeout=5)
        finally:
            if live.poll() is None:
                live.kill()
                live.communicate(timeout=5)
        assert live.returncode == -signal.SIGKILL
        complete = (prefix + remainder).split(b"\n")[:-1]
        killed_path = root / "killed.ndjson"
        killed_path.write_bytes(b"\n".join(complete) + b"\n")
        assert len(load_summary_series(killed_path).rows) == len(complete)
        damaged = root / "tail.ndjson"
        damaged.write_bytes(killed_path.read_bytes() + b'{"schema_version":9,"interval":')
        try:
            load_summary_series(damaged)
        except SystemExit:
            pass
        else:
            raise AssertionError("strict loading accepted a truncated record")
        assert len(load_summary_series(damaged, recover_tail=True).rows) == len(complete)

        # Synchronize on baseline source ownership before terminating an empty
        # capture; an orderly exit must not emit array delimiters or a record.
        empty = subprocess.Popen(
            [binary, "-P", "-s", "ctx_switches", "-f", "ndjson",
             "-i", "2", "-n", "0"], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 10
            while True:
                assert empty.poll() is None
                sources = []
                for fd in Path(f"/proc/{empty.pid}/fd").iterdir():
                    try:
                        sources.append(os.readlink(fd))
                    except FileNotFoundError:
                        pass
                if "/proc/stat" in sources:
                    break
                assert time.monotonic() < deadline, "baseline source was not opened"
                time.sleep(0.005)
            empty.terminate()
            output, error = empty.communicate(timeout=5)
            assert empty.returncode == 0, error
            assert output == b"", output
        finally:
            if empty.poll() is None:
                empty.kill()
                empty.communicate(timeout=5)

        broken_pipe = subprocess.Popen(
            [binary, "-P", "-s", "ctx_switches", "-f", "ndjson",
             "-i", "0.01", "-n", "2"], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        broken_pipe.stdout.close()
        try:
            assert broken_pipe.wait(timeout=10) == 1
        finally:
            if broken_pipe.poll() is None:
                broken_pipe.kill()
                broken_pipe.wait(timeout=5)
            broken_pipe.stderr.close()
    print("NDJSON ARM64 runtime tests passed.")


if __name__ == "__main__":
    main()
