#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Minimal smoke tests for armstat plotting loaders.

These tests intentionally avoid matplotlib and only validate that current
machine-readable exports can be parsed by the helper scripts.
"""

from __future__ import annotations

import importlib.util
import json
import math
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
armstat_loader = sys.modules["armstat_loader"]


class PlotLoaderTests(unittest.TestCase):
    def test_field_units_cover_power_and_lpi_usage(self):
        self.assertEqual(plot_sum.field_axis_label(["power"]), "power (mW)")
        self.assertEqual(
            plot_cpu.field_axis_label(["lpi0_usage"]), "lpi0_usage (/s)"
        )
        self.assertEqual(
            plot_sum.field_axis_label(["unknown", "power"]),
            "unknown, power (mW)",
        )

    def test_smoothing_preserves_missing_sample_gaps(self):
        values = plot_sum.smooth_series([10.0, 20.0, math.nan, 40.0], 3)

        self.assertEqual(values[:2], [10.0, 15.0])
        self.assertTrue(math.isnan(values[2]))
        self.assertEqual(values[3], 30.0)

    def test_non_finite_values_are_not_exposed_as_fields(self):
        fields = armstat_loader.collect_numeric_fields([
            {"power": float("inf"), "temp0": 45.0}
        ])

        self.assertNotIn("power", fields)
        self.assertIn("temp0", fields)

    def test_non_finite_values_become_plot_gaps(self):
        rows = [
            {"power": 10.0},
            {"power": float("inf")},
            {"power": "-inf"},
            {"power": 40.0},
        ]

        values = plot_sum.extract_series(rows, "power")
        smoothed = plot_sum.smooth_series(values, 2)

        self.assertEqual(values[0], 10.0)
        self.assertTrue(math.isnan(values[1]))
        self.assertTrue(math.isnan(values[2]))
        self.assertEqual(values[3], 40.0)
        self.assertEqual(smoothed[0], 10.0)
        self.assertTrue(math.isnan(smoothed[1]))
        self.assertTrue(math.isnan(smoothed[2]))
        self.assertEqual(smoothed[3], 40.0)

    def test_non_finite_topology_values_are_not_used_as_groups(self):
        series = plot_cpu.CpuSeriesData(
            x_values=[1, 2],
            x_label="sample",
            samples=[
                {0: {"node": float("inf"), "freq": 2000.0}},
                {0: {"node": 1, "freq": 2100.0}},
            ],
            cpu_ids=[0],
            numeric_fields=["freq", "node"],
        )

        groups, group_label = plot_cpu.build_cpu_groups(series, [0], "node")

        self.assertEqual(group_label, "node")
        self.assertEqual(groups, {1: [0]})

    def test_partial_timestamps_fall_back_to_one_sample_axis(self):
        data = [
            {
                "schema_version": 8,
                "interval": 1,
                "timestamp": 1774665600,
                "summary": {"freq": 2100.0},
            },
            {
                "schema_version": 8,
                "interval": 2,
                "timestamp": None,
                "summary": {"freq": 2200.0},
            },
        ]

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            series = plot_sum.load_summary_series(path)

        self.assertEqual(series.x_label, "sample")
        self.assertEqual(series.x_values, [1, 2])

    def test_missing_and_malformed_inputs_fail_cleanly(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            missing = Path(tmpdir) / "missing.json"
            malformed = Path(tmpdir) / "malformed.json"
            malformed.write_text("[{", encoding="utf-8")

            with self.assertRaisesRegex(SystemExit, "Could not read"):
                plot_sum.load_summary_series(missing)
            with self.assertRaisesRegex(SystemExit, "Could not read"):
                plot_sum.load_summary_series(malformed)

    def test_fractional_cpu_ids_are_rejected(self):
        data = [{
            "schema_version": 8,
            "interval": 1,
            "cpus": [{"cpu": 0.5, "freq": 2200.0}],
        }]

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "cpus.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, "invalid CPU ID"):
                plot_cpu.load_cpu_series(path)

    def test_schema_version_is_required_and_integral(self):
        invalid_versions = (None, 8.9, "8.5", float("inf"))

        for version in invalid_versions:
            item = {
                "interval": 1,
                "timestamp": 1774665600,
                "summary": {"freq": 2200.0},
            }
            if version is not None:
                item["schema_version"] = version

            with self.subTest(version=version), tempfile.TemporaryDirectory() as tmpdir:
                path = Path(tmpdir) / "summary.json"
                path.write_text(json.dumps([item]), encoding="utf-8")
                with self.assertRaises(SystemExit):
                    plot_sum.load_summary_series(path)

    def test_csv_schema_version_is_required(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,Scope,freq\n"
            ",1,1774665600,2026-03-28T10:40:00+0800,SUM,2200\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.csv"
            path.write_text(content, encoding="utf-8")
            with self.assertRaises(SystemExit):
                plot_sum.load_summary_series(path)

    def test_legacy_summary_csv_layout_is_rejected(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,SUM,freq\n"
            "8,1,1774665600,2026-03-28T10:40:00+0800,SUM,2200\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.csv"
            path.write_text(content, encoding="utf-8")
            with self.assertRaises(SystemExit):
                plot_sum.load_summary_series(path)

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

    def test_summary_presets_reject_missing_required_fields(self):
        series = plot_sum.SeriesData(
            x_values=[1],
            x_label="sample",
            rows=[{"temp0": 45.0}],
            numeric_fields=["temp0"],
        )

        for preset in ("freq", "power", "power-temp", "idle-lpi", "sysstat"):
            with self.subTest(preset=preset), self.assertRaises(SystemExit):
                plot_sum.resolve_preset(series, preset)

    def test_json_sample_range_recomputes_available_fields(self):
        data = [
            {
                "schema_version": 8,
                "interval": 1,
                "summary": {"freq": 2200.0},
            },
            {
                "schema_version": 8,
                "interval": 2,
                "summary": {"freq": None, "power": 120000.0},
            },
        ]

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            series = plot_sum.load_summary_series(path)
            series = plot_sum.slice_summary_series(series, "2:2")

        self.assertNotIn("freq", series.numeric_fields)
        self.assertIn("power", series.numeric_fields)

    def test_sample_range_rejects_start_beyond_input(self):
        series = plot_sum.SeriesData(
            x_values=[1, 2],
            x_label="sample",
            rows=[{"freq": 2100.0}, {"freq": 2200.0}],
            numeric_fields=["freq"],
        )

        with self.assertRaises(SystemExit):
            plot_sum.slice_summary_series(series, "3:4")

    def test_sysstat_preset_treats_memory_bandwidth_as_optional(self):
        fields = ["ctx_switches", "interrupts", "soft_interrupts"]
        series = plot_sum.SeriesData(
            x_values=[1],
            x_label="sample",
            rows=[{field: 1 for field in fields}],
            numeric_fields=fields,
        )

        left_fields, right_fields, _ = plot_sum.resolve_preset(series, "sysstat")

        self.assertEqual(left_fields, fields)
        self.assertEqual(right_fields, [])

    def test_summary_csv_loader_accepts_canonical_fields(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,Scope,freq,busy_percent\n"
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
            "timestamp_iso,Scope,freq,busy_percent\n"
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

    def test_summary_csv_loader_accepts_high_lpi_columns(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,Scope,lpi4\n"
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

    def test_cpu_csv_loader_accepts_canonical_fields(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,CPU,freq,busy_percent,temp\n"
            "8,1,1774665600,2026-03-28T10:40:00+0800,0,2200.00,1.00,45.00\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "cpus.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_cpu.load_cpu_series(path)

        self.assertEqual(series.cpu_ids, [0])
        self.assertEqual(series.x_label, "time")
        self.assertIn("temp", series.numeric_fields)

    def test_cpu_csv_loader_accepts_high_lpi_usage_columns(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,CPU,lpi4_usage\n"
            "8,1,1774665600,2026-03-28T10:40:00+0800,0,12.50\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "cpus.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_cpu.load_cpu_series(path)

        self.assertIn("lpi4_usage", series.numeric_fields)
        self.assertEqual(
            plot_cpu.resolve_field_name("idle_state_usage4", series.numeric_fields),
            "lpi4_usage",
        )

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

    def test_cpu_csv_range_exposes_only_selected_cpu_ids(self):
        content = (
            "schema_version,interval,timestamp,timestamp_ns,timestamp_iso,CPU,freq\n"
            "8,1,1774665600,1774665600100000000,,0,2100\n"
            "8,2,1774665600,1774665600200000000,,1,2200\n"
            "8,3,1774665600,1774665600300000000,,2,2300\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "cpus.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_cpu.load_cpu_series(path, "2:2")

        self.assertEqual(series.cpu_ids, [1])
        self.assertEqual(len(series.samples), 1)
        self.assertEqual(set(series.samples[0]), {1})

    def test_core_groups_include_package_identity(self):
        series = plot_cpu.CpuSeriesData(
            x_values=[1],
            x_label="sample",
            samples=[{
                0: {"package": 0, "core": 0, "freq": 2000.0},
                1: {"package": 0, "core": 0, "freq": 2100.0},
                64: {"package": 1, "core": 0, "freq": 2200.0},
            }],
            cpu_ids=[0, 1, 64],
            numeric_fields=["package", "core", "freq"],
        )

        groups, group_label = plot_cpu.build_cpu_groups(
            series, series.cpu_ids, "core"
        )

        self.assertEqual(group_label, "core")
        self.assertEqual(groups, {(0, 0): [0, 1], (1, 0): [64]})
        self.assertEqual(
            plot_cpu.format_group_name(group_label, (1, 0)), "pkg1/core0"
        )

    def test_selected_cpus_must_have_plot_data(self):
        series = plot_cpu.CpuSeriesData(
            x_values=[1],
            x_label="sample",
            samples=[{0: {"freq": None}, 1: {"freq": 2200.0}}],
            cpu_ids=[0, 1],
            numeric_fields=["freq"],
        )

        with self.assertRaises(SystemExit):
            plot_cpu.require_cpu_field_data(series, [0], ["freq"])

        plot_cpu.require_cpu_field_data(series, [1], ["freq"])

    def test_cpus_and_groups_without_primary_data_are_filtered(self):
        series = plot_cpu.CpuSeriesData(
            x_values=[1],
            x_label="sample",
            samples=[{
                0: {"package": 0, "core": 0, "freq": None},
                1: {"package": 0, "core": 1, "freq": 2200.0},
            }],
            cpu_ids=[0, 1],
            numeric_fields=["package", "core", "freq"],
        )

        cpu_ids, skipped = plot_cpu.filter_cpu_ids_with_data(
            series, series.cpu_ids, "freq"
        )
        groups, _ = plot_cpu.build_cpu_groups(
            series, series.cpu_ids, "core"
        )
        groups, skipped_groups = plot_cpu.filter_groups_with_data(
            series, groups, "freq"
        )

        self.assertEqual(cpu_ids, [1])
        self.assertEqual(skipped, [0])
        self.assertEqual(groups, {(0, 1): [1]})
        self.assertEqual(skipped_groups, [(0, 0)])


if __name__ == "__main__":
    unittest.main()
