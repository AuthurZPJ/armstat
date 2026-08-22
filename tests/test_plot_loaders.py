#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Minimal smoke tests for armstat plotting loaders.

These tests intentionally avoid matplotlib and only validate that current
machine-readable exports can be parsed by the helper scripts.
"""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load module from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


plot_sum = load_module("plot_sum", SCRIPTS / "plot_sum.py")
plot_cpu = load_module("plot_cpu", SCRIPTS / "plot_cpu.py")


class PlotLoaderTests(unittest.TestCase):
    def test_summary_json_loader_accepts_schema_version(self):
        data = [
            {
                "schema_version": 8,
                "interval": 1,
                "duration_us": 1000123,
                "timestamp": 1774665600,
                "timestamp_ns": 1774665600123456789,
                "summary": {
                    "freq": 2200.0,
                    "uncore_freq": 1600.0,
                    "busy_percent": 1.0,
                    "temp0": 45.0,
                },
            }
        ]

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            series = plot_sum.load_summary_series(path)

        self.assertEqual(series.x_label, "time")
        self.assertIn("freq", series.numeric_fields)
        self.assertIn("uncore_freq", series.numeric_fields)
        self.assertNotIn("duration_us", series.numeric_fields)
        self.assertEqual(len(series.rows), 1)
        self.assertEqual(series.x_values[0].microsecond, 123457)
        self.assertEqual(
            plot_sum.resolve_field_name("freq", series.numeric_fields),
            "freq",
        )

    def test_summary_freq_preset_includes_uncore_when_available(self):
        data = [
            {
                "schema_version": 8,
                "interval": 1,
                "timestamp": 1774665600,
                "summary": {
                    "freq": 2200.0,
                    "uncore_freq": 1600.0,
                },
            }
        ]

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            series = plot_sum.load_summary_series(path)
            left_fields, right_fields, _ = plot_sum.resolve_preset(series, "freq")

        self.assertEqual(left_fields, ["freq", "uncore_freq"])
        self.assertEqual(right_fields, [])

    def test_summary_csv_loader_ignores_schema_version_column(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,SUM,Freq,Busy%\n"
            "8,1,1774665600,2026-03-28T10:40:00+0800,SUM,2200.00,1.00\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_sum.load_summary_series(path)

        self.assertEqual(series.x_label, "time")
        self.assertEqual(len(series.rows), 1)
        self.assertIn("freq", series.numeric_fields)

    def test_summary_csv_loader_accepts_current_scope_and_duration(self):
        content = (
            "schema_version,interval,duration_us,timestamp,timestamp_ns,"
            "timestamp_iso,Scope,Freq,Busy%\n"
            "8,1,1000123,1774665600,1774665600123456789,"
            "2026-03-28T10:40:00.123456789+08:00,SUM,2200.00,1.00\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_sum.load_summary_series(path)

        self.assertEqual(len(series.rows), 1)
        self.assertIn("freq", series.numeric_fields)
        self.assertNotIn("duration_us", series.numeric_fields)
        self.assertEqual(
            plot_sum.resolve_field_name("freq", series.numeric_fields),
            "freq",
        )

    def test_summary_csv_loader_canonicalizes_high_lpi_columns(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,SUM,LPI-4\n"
            "8,1,1774665600,2026-03-28T10:40:00+0800,SUM,12.50\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_sum.load_summary_series(path)

        self.assertIn("lpi4", series.numeric_fields)
        self.assertEqual(plot_sum.resolve_field_name("lpi-4", series.numeric_fields), "lpi4")

    def test_summary_csv_loader_accepts_mixed_scope_csv(self):
        content = (
            "schema_version,interval,timestamp,timestamp_ns,timestamp_iso,Scope,CPU,Package,summary.power,package.freq,cpu.freq,summary.pmu.cycles,cpu.pmu.cycles\n"
            "8,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,SUM,,,120000,,,999,\n"
            "8,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,PKG,,0,,2200.00,,,\n"
            "8,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,CPU,0,,,,2200.00,,123\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "mixed.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_sum.load_summary_series(path)

        self.assertEqual(len(series.rows), 1)
        self.assertIn("power", series.numeric_fields)
        self.assertIn("pmu.cycles", series.numeric_fields)

    def test_summary_csv_range_counts_only_summary_rows(self):
        content = (
            "schema_version,interval,timestamp,timestamp_ns,timestamp_iso,Scope,CPU,Package,summary.power,package.freq,cpu.freq\n"
            "8,1,1774665600,1774665600100000000,,SUM,,,100,,\n"
            "8,1,1774665600,1774665600100000000,,PKG,,0,,2200,\n"
            "8,1,1774665600,1774665600100000000,,CPU,0,,,,2200\n"
            "8,2,1774665600,1774665600200000000,,SUM,,,200,,\n"
            "8,2,1774665600,1774665600200000000,,PKG,,0,,2300,\n"
            "8,2,1774665600,1774665600200000000,,CPU,0,,,,2300\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "mixed.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_sum.load_summary_series(path, "2:2")

        self.assertEqual(len(series.rows), 1)
        self.assertEqual(series.rows[0]["power"], "200")

    def test_cpu_json_loader_accepts_schema_version(self):
        data = [
            {
                "schema_version": 8,
                "interval": 1,
                "timestamp": 1774665600,
                "cpus": [
                    {
                        "cpu": 0,
                        "freq": 2200.0,
                        "busy_percent": 1.0,
                        "pmu": {"cycles": 123},
                    }
                ],
            }
        ]

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "cpus.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            series = plot_cpu.load_cpu_series(path)

        self.assertEqual(series.cpu_ids, [0])
        self.assertEqual(series.x_label, "time")
        self.assertIn("freq", series.numeric_fields)
        self.assertEqual(
            plot_cpu.resolve_field_name("freq", series.numeric_fields), "freq"
        )

    def test_cpu_csv_loader_ignores_schema_version_column(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,CPU,Freq,Busy%,Temp\n"
            "8,1,1774665600,2026-03-28T10:40:00+0800,0,2200.00,1.00,45.00\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "cpus.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_cpu.load_cpu_series(path)

        self.assertEqual(series.cpu_ids, [0])
        self.assertEqual(series.x_label, "time")
        self.assertIn("temp", series.numeric_fields)

    def test_cpu_csv_loader_canonicalizes_high_lpi_columns(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,CPU,LPI-4\n"
            "8,1,1774665600,2026-03-28T10:40:00+0800,0,12.50\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "cpus.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_cpu.load_cpu_series(path)

        self.assertIn("lpi4", series.numeric_fields)
        self.assertEqual(plot_cpu.resolve_field_name("lpi-4", series.numeric_fields), "lpi4")

    def test_cpu_csv_loader_accepts_mixed_scope_csv(self):
        content = (
            "schema_version,interval,timestamp,timestamp_ns,timestamp_iso,Scope,CPU,Package,summary.power,package.freq,cpu.freq,summary.pmu.cycles,cpu.pmu.cycles\n"
            "8,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,SUM,,,120000,,,999,\n"
            "8,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,PKG,,0,,2200.00,,,\n"
            "8,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,CPU,0,,,,2200.00,,123\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "mixed.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_cpu.load_cpu_series(path)

        self.assertEqual(series.cpu_ids, [0])
        self.assertIn("freq", series.numeric_fields)
        self.assertIn("pmu.cycles", series.numeric_fields)


if __name__ == "__main__":
    unittest.main()
