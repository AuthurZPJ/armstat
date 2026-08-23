#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Render smoke tests for the optional armstat plotting dependency."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def run_plot(arguments: list[str]):
    environment = os.environ.copy()
    environment["MPLBACKEND"] = "Agg"
    return subprocess.run(
        [sys.executable, *arguments],
        cwd=ROOT,
        env=environment,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def main() -> int:
    if importlib.util.find_spec("matplotlib") is None:
        if os.environ.get("ARMSTAT_REQUIRE_PLOT_RENDER") == "1":
            print(
                "plot render tests: FAIL "
                "(ARMSTAT_REQUIRE_PLOT_RENDER=1 but matplotlib is not installed)",
                file=sys.stderr,
            )
            return 1
        print("plot render tests: SKIP (matplotlib is not installed)")
        return 0

    with tempfile.TemporaryDirectory() as tmpdir:
        output_dir = Path(tmpdir)
        summary_path = output_dir / "summary.json"
        cpu_path = output_dir / "cpus.json"
        summary_output = output_dir / "plots" / "summary.png"
        cpu_output = output_dir / "plots" / "cpus.svg"

        summary_path.write_text(json.dumps([
            {
                "schema_version": 8,
                "interval": interval,
                "duration_us": 1_000_000,
                "timestamp": 1_774_665_600 + interval,
                "timestamp_ns": (1_774_665_600 + interval) * 1_000_000_000,
                "summary": {
                    "power": 120_000 + interval * 100,
                    "temp0": 45.0 + interval,
                },
            }
            for interval in range(1, 4)
        ]), encoding="utf-8")

        cpu_path.write_text(json.dumps([
            {
                "schema_version": 8,
                "interval": interval,
                "duration_us": 1_000_000,
                "timestamp": 1_774_665_600 + interval,
                "timestamp_ns": (1_774_665_600 + interval) * 1_000_000_000,
                "cpus": [
                    {"cpu": 0, "package": 0, "core": 0,
                     "freq": 2_000 + interval * 10,
                     "temp": 45.0 + interval},
                    {"cpu": 64, "package": 1, "core": 0,
                     "freq": 2_100 + interval * 10},
                    {"cpu": 2, "package": 0, "core": 2,
                     "freq": None},
                ],
            }
            for interval in range(1, 4)
        ]), encoding="utf-8")

        run_plot([
            "scripts/plot_sum.py", str(summary_path),
            "--preset", "power-temp", "-o", str(summary_output),
        ])
        cpu_result = run_plot([
            "scripts/plot_cpu.py", str(cpu_path),
            "--y", "freq", "--y2", "temp", "--group-by", "core",
            "-o", str(cpu_output),
        ])

        assert summary_output.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
        assert summary_output.stat().st_size > 1_000
        svg = cpu_output.read_text(encoding="utf-8")
        assert "<svg" in svg[:1_000]
        assert "pkg0/core0:freq" in svg
        assert "pkg1/core0:freq" in svg
        assert "pkg0/core0:temp" in svg
        assert "pkg1/core0:temp" not in svg
        assert "freq (MHz)" in svg
        assert "pkg0/core2:freq" not in svg
        assert "skipping groups" in cpu_result.stderr
        assert "skipping secondary 'temp' lines" in cpu_result.stderr

    print("plot render tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
