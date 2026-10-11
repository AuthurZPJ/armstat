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
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


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
    def test_json_streaming_handles_chunk_boundaries_without_whole_file_read(self):
        padding = ('brackets ]},[ and quote " and backslash \\ ' * 4000) + "汉字"
        data = [
            {"schema_version": 9, "interval": index,
             "summary": {"freq": index, "nested": {"padding": padding}},
             "cpus": [{"cpu": 0, "freq": index, "padding": padding}]}
            for index in range(1, 4)
        ]
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "chunks.json"
            path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
            reads = []
            original_open = Path.open

            class BoundedReader:
                def __init__(self, handle):
                    self.handle = handle

                def __enter__(self):
                    return self

                def __exit__(self, *args):
                    self.handle.close()

                def read(self, size=-1):
                    reads.append(size)
                    if size <= 0:
                        raise AssertionError("unbounded JSON read")
                    return self.handle.read(size)

            def open_bounded(source, *args, **kwargs):
                handle = original_open(source, *args, **kwargs)
                return BoundedReader(handle) if source == path else handle

            with patch.object(Path, "open", open_bounded), patch.object(
                Path, "read_text", side_effect=AssertionError("whole-file JSON read")
            ):
                summary = plot_sum.load_summary_series(path, "2:2", fields=["freq"])
                cpus = plot_cpu.load_cpu_series(path, "2:2", cpu_filter=[0], fields=["freq"])

        self.assertGreater(len(reads), 4)
        self.assertEqual(reads[0], 64 * 1024)
        self.assertLess(max(reads), len(json.dumps(data, ensure_ascii=False)) // 2)
        self.assertEqual(summary.x_values, [2])
        self.assertEqual(summary.rows[0]["freq"], 2)
        self.assertNotIn("nested.padding", summary.rows[0])
        self.assertEqual(cpus.samples, [{0: {"freq": 2}}])

    def test_incremental_json_matches_decoder_across_literal_and_escape_boundaries(self):
        data = [{"schema_version": 9, "interval": index, "summary": {
            "freq": 1.234e123, "signed": -1.234e-123, "fraction": 1.234,
            "truth": True, "false": False, "missing": None,
            '[]"\\key': [{}, [], 'quote " and backslash \\ and unicode 汉字', '\n\t\u1234'],
        }} for index in range(1, 4)]
        original_open = Path.open
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "boundaries.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            for limit in range(1, 65):
                class ShortReader:
                    def __enter__(self):
                        self.handle = original_open(path, "r", encoding="utf-8")
                        return self

                    def __exit__(self, *args):
                        self.handle.close()

                    def read(self, size):
                        return self.handle.read(min(size, limit))

                with self.subTest(chunk=limit), patch.object(
                    Path, "open", lambda *args, **kwargs: ShortReader()
                ):
                    self.assertEqual(list(armstat_loader.iter_json_samples(path)), data)

    def test_json_projection_still_rejects_damage_outside_selected_window(self):
        first = '{"schema_version":9,"interval":1,"summary":{"freq":1}}'
        padding = json.dumps('escaped " \\ ],{} ' * 7000)
        bad_samples = [
            '7',
            '{"schema_version":8,"interval":2,"summary":{}}',
            '{"schema_version":9,"interval":3,"summary":{}}',
            '{"schema_version":9,"interval":2,"cpus":[]}',
            '{"schema_version":9,"interval":2,"summary":{"freq":NaN}}',
            '{"schema_version":9,"interval":2,"summary":{"padding":' + padding +
            ',"freq":1,"freq":2}}',
        ]
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "bad.json"
            for second in bad_samples:
                with self.subTest(second=second[:70]):
                    path.write_text(f"[{first},{second}]", encoding="utf-8")
                    with self.assertRaises(SystemExit):
                        plot_sum.load_summary_series(path, "1:1", fields=["freq"])
            for tail in (",]", "] trailing", "][]", "", ","):
                with self.subTest(tail=tail):
                    path.write_text("[" + first + tail, encoding="utf-8")
                    with self.assertRaises(SystemExit):
                        plot_sum.load_summary_series(path, "1:1", fields=["freq"])

    def test_cpu_projection_does_not_hide_invalid_or_duplicate_cpu_ids(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            for suffix in ("json", "csv"):
                path = Path(tmpdir) / f"cpus.{suffix}"
                for bad_id in (-1, 1.5, 1):
                    with self.subTest(suffix=suffix, cpu=bad_id):
                        if suffix == "json":
                            path.write_text(json.dumps([
                                {"schema_version": 9, "interval": 1,
                                 "cpus": [{"cpu": 0, "freq": 100}]},
                                {"schema_version": 9, "interval": 2,
                                 "cpus": [{"cpu": 1, "freq": 200},
                                          {"cpu": bad_id, "freq": 300}]},
                            ]), encoding="utf-8")
                        else:
                            path.write_text(
                                "schema_version,interval,CPU,freq\n"
                                "9,1,0,100\n9,2,1,200\n" + f"9,2,{bad_id},300\n",
                                encoding="utf-8",
                            )
                        with self.assertRaises(SystemExit):
                            plot_cpu.load_cpu_series(path, "1:1", cpu_filter=[0], fields=["freq"])

    def test_csv_projection_checks_schema_and_interval_after_selected_window(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "records.csv"
            for loader, header, first, bad_rows in (
                (plot_sum.load_summary_series, "schema_version,interval,Scope,freq\n",
                 "9,1,SUM,100\n", ("8,2,SUM,200\n", "9,3,SUM,200\n", "9,2,SUM\n")),
                (plot_cpu.load_cpu_series, "schema_version,interval,CPU,freq\n",
                 "9,1,0,100\n", ("8,2,1,200\n", "9,3,1,200\n", "9,2,1\n")),
            ):
                for bad in bad_rows:
                    with self.subTest(loader=loader.__name__, bad=bad):
                        path.write_text(header + first + bad, encoding="utf-8")
                        with self.assertRaises(SystemExit):
                            loader(path, "1:1", fields=["freq"])

    def test_streaming_ranges_preserve_bounds_and_original_sample_indices(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            for suffix in ("json", "csv"):
                path = Path(tmpdir) / f"records.{suffix}"
                if suffix == "json":
                    path.write_text(json.dumps([
                        {"schema_version": 9, "interval": index,
                         "summary": {"freq": index}, "cpus": [{"cpu": 0, "freq": index}]}
                        for index in range(1, 4)
                    ]), encoding="utf-8")
                else:
                    path.write_text(
                        "schema_version,interval,Scope,CPU,Package,summary.freq,cpu.freq\n" +
                        "".join(f"9,{i},SUM,,,{i},\n9,{i},CPU,0,,,{i}\n" for i in range(1, 4)),
                        encoding="utf-8",
                    )
                for expr, expected in (("2:", [2, 3]), (":2", [1, 2]), ("2-9", [2, 3])):
                    with self.subTest(suffix=suffix, expr=expr):
                        self.assertEqual(plot_sum.load_summary_series(path, expr).x_values, expected)
                        self.assertEqual(plot_cpu.load_cpu_series(path, expr).x_values, expected)
                for loader in (plot_sum.load_summary_series, plot_cpu.load_cpu_series):
                    with self.assertRaisesRegex(SystemExit, "contains only 3 samples"):
                        loader(path, "4:9")
                    for expr in ("0:2", "3:2", "x:2", "2"):
                        with self.subTest(suffix=suffix, expr=expr), self.assertRaises(SystemExit):
                            loader(path, expr)

    def test_cpu_and_field_projection_preserves_aliases_and_scope(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            for suffix in ("json", "csv"):
                path = Path(tmpdir) / f"records.{suffix}"
                if suffix == "json":
                    path.write_text(json.dumps([{
                        "schema_version": 9, "interval": 1,
                        "summary": {"freq": 99, "power": 1000},
                        "cpus": [{"cpu": 0, "freq": 100, "package": 0, "temp": 40,
                                  "pmu": {"cycles": 10, "instructions": 20}},
                                 {"cpu": 1, "freq": 200, "package": 1, "temp": 50,
                                  "pmu": {"cycles": 30, "instructions": 40}}],
                    }]), encoding="utf-8")
                else:
                    path.write_text(
                        "schema_version,interval,Scope,CPU,Package,summary.freq,summary.power,"
                        "cpu.freq,cpu.package,cpu.temp,cpu.pmu.cycles,cpu.pmu.instructions\n"
                        "9,1,SUM,,,99,1000,,,,,\n"
                        "9,1,CPU,0,,,,100,0,40,10,20\n"
                        "9,1,CPU,1,,,,200,1,50,30,40\n", encoding="utf-8",
                    )
                summary = plot_sum.load_summary_series(path, fields=["summary_freq_mhz"])
                cpus = plot_cpu.load_cpu_series(path, cpu_filter=[1], fields=["Freq", "cycles"])
                self.assertEqual(summary.numeric_fields, ["freq"])
                self.assertEqual(cpus.cpu_ids, [1])
                self.assertEqual(cpus.numeric_fields, ["freq", "pmu.cycles"])
                self.assertEqual(set(cpus.samples[0][1]), {"freq", "pmu.cycles"})
                self.assertEqual(plot_cpu.extract_series(cpus.samples, 1, "freq"), [200])

    def test_unmatched_cpu_filter_is_reported_before_missing_projected_fields(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            for suffix in ("json", "csv"):
                path = Path(tmpdir) / f"cpus.{suffix}"
                if suffix == "json":
                    path.write_text(json.dumps([{
                        "schema_version": 9, "interval": 1,
                        "cpus": [{"cpu": 0, "freq": 2200}],
                    }]), encoding="utf-8")
                else:
                    path.write_text("schema_version,interval,CPU,freq\n9,1,0,2200\n",
                                    encoding="utf-8")
                result = subprocess.run(
                    [sys.executable, str(SCRIPTS / "plot_cpu.py"), str(path),
                     "--y", "freq", "--cpu-filter", "99"],
                    text=True, capture_output=True,
                )
                self.assertEqual(result.returncode, 1)
                self.assertIn("CPU filter does not match any CPU", result.stderr)
                self.assertNotIn("Unknown field", result.stderr)

    def test_rolling_smoothing_preserves_window_gaps_and_has_linear_scan_work(self):
        import plot_utils

        values = [math.nan if i % 23 == 0 else math.inf if i % 29 == 0 else (i % 11 - 5) * 0.7
                  for i in range(2500)]
        for window in (1, 2, 17, 2048, 3000):
            calls = 0
            original_finite = math.isfinite

            def counted_finite(value):
                nonlocal calls
                calls += 1
                return original_finite(value)

            with patch.object(plot_utils.math, "isfinite", counted_finite):
                actual = plot_sum.smooth_series(values, window)
            self.assertLess(calls, 9 * len(values))
            for index, value in enumerate(values):
                if not math.isfinite(value):
                    self.assertTrue(math.isnan(actual[index]))
                    continue
                bucket = [v for v in values[max(0, index - window + 1):index + 1]
                          if math.isfinite(v)]
                self.assertAlmostEqual(actual[index], sum(bucket) / len(bucket), places=10)

        overflow = plot_sum.smooth_series([1e308, 1e308, 3.0, 4.0], 2)
        self.assertEqual(overflow[1], 1e308)
        self.assertEqual(overflow[-1], 3.5)
        three = plot_sum.smooth_series([1e308, 1e308, 1.0], 3)[-1]
        self.assertTrue(math.isfinite(three))
        self.assertAlmostEqual(three / 1e308, 2 / 3)
        cancelled = plot_sum.smooth_series([1e308, 1e308, -1e308, -1e308, 3.0, 4.0], 2)
        self.assertEqual(cancelled, [1e308, 1e308, 0.0, -1e308, -5e307, 3.5])
        self.assertEqual(plot_sum.smooth_series([1e100, 1.0, 2.0], 2)[-1], 1.5)

    def test_render_downsampling_keeps_extrema_and_missing_interval_boundaries(self):
        import plot_utils

        x_values = list(range(10000))
        values = [0.0] * len(x_values)
        peaks = [index + offset for index in range(0, 10000, 2000) for offset in (123, 999)]
        for index, point in enumerate(peaks):
            values[point] = 100.0 + index if index % 2 == 0 else -100.0 - index
        for index in range(4500, 4550):
            values[index] = math.nan
        reduced_x, reduced_y = plot_utils.downsample_series(x_values, values, max_points=20)
        self.assertLessEqual(len(reduced_x), 24)
        self.assertEqual(reduced_x[0], 0)
        self.assertEqual(reduced_x[-1], 9999)
        self.assertTrue(set(peaks).issubset(reduced_x))
        for point in (4499, 4500, 4549, 4550):
            self.assertIn(point, reduced_x)
        self.assertTrue(math.isnan(reduced_y[reduced_x.index(4500)]))
        self.assertEqual(list(reduced_x), sorted(set(reduced_x)))
        short_x = [1, 2]
        short_y = [1.0, math.nan]
        self.assertIs(plot_utils.downsample_series(short_x, short_y)[0], short_x)

    def test_nanosecond_timestamp_offset_is_preserved(self):
        for fraction in ("", ".1", ".123", ".123456", ".123456789"):
            for offset, seconds in (
                ("+08:00", 28800), ("+0800", 28800),
                ("-03:30", -12600), ("-0330", -12600), ("Z", 0),
            ):
                with self.subTest(fraction=fraction, offset=offset):
                    zone = armstat_loader.parse_timestamp_timezone(
                        f"2026-03-28T12:00:00{fraction}{offset}"
                    )
                    self.assertEqual(zone.utcoffset(None).total_seconds(), seconds)

    def test_oversized_json_integers_become_gaps(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "oversized.json"
            path.write_text(json.dumps([
                {"schema_version": 9, "interval": 1,
                 "summary": {"power": 10 ** 400}},
                {"schema_version": 9, "interval": 2,
                 "summary": {"power": 1000}},
            ]), encoding="utf-8")
            series = armstat_loader.load_summary_series(path)
            values = plot_sum.extract_series(series.rows, "power")
            self.assertTrue(math.isnan(values[0]))
            self.assertEqual(values[1], 1000)

    def test_plot_parent_failure_has_clean_error(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            parent = Path(tmpdir) / "not-a-directory"
            parent.write_text("keep me", encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, "Could not write plot"):
                plot_sum.save_figure(None, parent / "plot.png")
            self.assertEqual(parent.read_text(encoding="utf-8"), "keep me")

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

    def test_failed_plot_write_preserves_existing_destination(self):
        class FailingFigure:
            def savefig(self, path, **_kwargs):
                Path(path).write_bytes(b"partial")
                raise RuntimeError("render failed")

        with tempfile.TemporaryDirectory() as tmpdir:
            output = Path(tmpdir) / "plot.png"
            output.write_bytes(b"previous complete image")

            with self.assertRaisesRegex(SystemExit, "Could not write plot"):
                plot_sum.save_figure(FailingFigure(), output)

            self.assertEqual(output.read_bytes(), b"previous complete image")
            self.assertEqual(list(output.parent.glob(".plot.*.png")), [])

    def test_atomic_plot_write_preserves_destination_permissions(self):
        class SuccessfulFigure:
            def savefig(self, path, **_kwargs):
                Path(path).write_bytes(b"complete image")

        with tempfile.TemporaryDirectory() as tmpdir:
            output = Path(tmpdir) / "plot.png"
            output.write_bytes(b"previous image")
            os.chmod(output, 0o640)

            plot_sum.save_figure(SuccessfulFigure(), output)

            self.assertEqual(output.read_bytes(), b"complete image")
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o640)

            new_output = Path(tmpdir) / "new-plot.png"
            previous_umask = os.umask(0o027)
            try:
                plot_sum.save_figure(SuccessfulFigure(), new_output)
            finally:
                os.umask(previous_umask)

            self.assertEqual(stat.S_IMODE(new_output.stat().st_mode), 0o640)

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
                "schema_version": 9,
                "interval": 1,
                "timestamp": 1774665600,
                "summary": {"freq": 2100.0},
            },
            {
                "schema_version": 9,
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
            "schema_version": 9,
            "interval": 1,
            "cpus": [{"cpu": 0.5, "freq": 2200.0}],
        }]

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "cpus.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, "invalid CPU ID"):
                plot_cpu.load_cpu_series(path)

    def test_duplicate_cpu_ids_are_rejected(self):
        json_data = [{
            "schema_version": 9,
            "interval": 1,
            "cpus": [
                {"cpu": 4, "freq": 2100.0},
                {"cpu": 4, "freq": 2200.0},
            ],
        }]
        csv_data = (
            "schema_version,interval,timestamp,CPU,freq\n"
            "9,1,1774665600,4,2100\n"
            "9,1,1774665600,4,2200\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            json_path = Path(tmpdir) / "duplicate.json"
            csv_path = Path(tmpdir) / "duplicate.csv"
            json_path.write_text(json.dumps(json_data), encoding="utf-8")
            csv_path.write_text(csv_data, encoding="utf-8")

            with self.assertRaisesRegex(SystemExit, "duplicate CPU ID 4"):
                plot_cpu.load_cpu_series(json_path)
            with self.assertRaisesRegex(SystemExit, "duplicate CPU ID 4"):
                plot_cpu.load_cpu_series(csv_path)

    def test_malformed_csv_structure_is_rejected_cleanly(self):
        malformed_inputs = {
            "duplicate column": (
                "schema_version,interval,Scope,freq,freq\n"
                "9,1,SUM,2100,2200\n",
                "duplicate summary CSV columns: freq",
            ),
            "extra cell": (
                "schema_version,interval,Scope,freq\n"
                "9,1,SUM,2100,unexpected\n",
                "contains more cells than its CSV header",
            ),
            "missing cell": (
                "schema_version,interval,Scope,freq\n"
                "9,1,SUM\n",
                "contains fewer cells than its CSV header",
            ),
        }

        for name, (content, message) in malformed_inputs.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmpdir:
                path = Path(tmpdir) / "summary.csv"
                path.write_text(content, encoding="utf-8")
                with self.assertRaisesRegex(SystemExit, message):
                    plot_sum.load_summary_series(path)

    def test_csv_range_still_rejects_damage_outside_selected_window(self):
        content = (
            "schema_version,interval,Scope,freq\n"
            "9,1,SUM,2100\n"
            "9,2,SUM\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.csv"
            path.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, "contains fewer cells"):
                plot_sum.load_summary_series(path, "1:1")

    def test_noncontiguous_intervals_are_rejected(self):
        json_data = [
            {"schema_version": 9, "interval": 1, "summary": {"freq": 2100}},
            {"schema_version": 9, "interval": 3, "summary": {"freq": 2200}},
        ]
        csv_data = (
            "schema_version,interval,Scope,freq\n"
            "9,1,SUM,2100\n"
            "9,3,SUM,2200\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            json_path = Path(tmpdir) / "summary.json"
            csv_path = Path(tmpdir) / "summary.csv"
            json_path.write_text(json.dumps(json_data), encoding="utf-8")
            csv_path.write_text(csv_data, encoding="utf-8")

            with self.assertRaisesRegex(SystemExit, "expected 2"):
                plot_sum.load_summary_series(json_path)
            with self.assertRaisesRegex(SystemExit, "expected 2"):
                plot_sum.load_summary_series(csv_path)

    def test_malformed_json_structure_is_rejected_cleanly(self):
        malformed_inputs = {
            "non-object sample": (
                '[{"schema_version": 9, "interval": 1, '
                '"summary": {"freq": 2100}}, 7]',
                plot_sum.load_summary_series,
                "sample 2 is not a JSON object",
            ),
            "missing summary": (
                '[{"schema_version": 9, "interval": 1, "cpus": []}]',
                plot_sum.load_summary_series,
                "does not contain a summary object",
            ),
            "missing cpus": (
                '[{"schema_version": 9, "interval": 1, "summary": {}}]',
                plot_cpu.load_cpu_series,
                "does not contain a cpus array",
            ),
            "non-object CPU row": (
                '[{"schema_version": 9, "interval": 1, "cpus": [4]}]',
                plot_cpu.load_cpu_series,
                "CPU entry 1 is not a JSON object",
            ),
            "empty CPU rows": (
                '[{"schema_version": 9, "interval": 1, "cpus": []}]',
                plot_cpu.load_cpu_series,
                "does not contain any CPU rows",
            ),
            "duplicate JSON key": (
                '[{"schema_version": 9, "summary": '
                '{"freq": 2100, "freq": 2200}}]',
                plot_sum.load_summary_series,
                "duplicate JSON key 'freq'",
            ),
            "non-standard JSON number": (
                '[{"schema_version": 9, "summary": {"freq": NaN}}]',
                plot_sum.load_summary_series,
                "non-standard JSON constant 'NaN'",
            ),
        }

        for name, (content, loader, message) in malformed_inputs.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as tmpdir:
                path = Path(tmpdir) / "input.json"
                path.write_text(content, encoding="utf-8")
                with self.assertRaisesRegex(SystemExit, message):
                    loader(path)

    def test_lpi_values_above_100_survive_json_and_csv(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            json_path = Path(tmpdir) / "lpi.json"
            csv_path = Path(tmpdir) / "lpi.csv"
            json_path.write_text(json.dumps([{
                "schema_version": 9, "interval": 1,
                "summary": {"idle_percent": 80, "lpi0": 125},
                "cpus": [{"cpu": 0, "lpi0": 200}],
            }]), encoding="utf-8")
            csv_path.write_text(
                "schema_version,interval,Scope,CPU,Package,summary.lpi0,cpu.lpi0\n"
                "9,1,SUM,,,125,\n"
                "9,1,CPU,0,,,200\n", encoding="utf-8",
            )
            for path in (json_path, csv_path):
                summary = plot_sum.load_summary_series(path)
                cpus = plot_cpu.load_cpu_series(path)
                self.assertEqual(plot_sum.extract_series(summary.rows, "lpi0"), [125])
                self.assertEqual(float(cpus.samples[0][0]["lpi0"]), 200)

    def test_old_residual_lpi_schema_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            json_path = Path(tmpdir) / "old.json"
            csv_path = Path(tmpdir) / "old.csv"
            json_path.write_text(json.dumps([{
                "schema_version": 8, "interval": 1,
                "summary": {"lpi0": 80},
                "cpus": [{"cpu": 0, "lpi0": 80}],
            }]), encoding="utf-8")
            csv_path.write_text(
                "schema_version,interval,Scope,CPU,Package,summary.lpi0,cpu.lpi0\n"
                "8,1,SUM,,,80,\n"
                "8,1,CPU,0,,,80\n", encoding="utf-8",
            )
            for path in (json_path, csv_path):
                for loader in (plot_sum.load_summary_series,
                               plot_cpu.load_cpu_series):
                    with self.assertRaisesRegex(SystemExit, "schema_version=8"):
                        loader(path)

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
            "9,1,1774665600,2026-03-28T10:40:00+0800,SUM,2200\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.csv"
            path.write_text(content, encoding="utf-8")
            with self.assertRaises(SystemExit):
                plot_sum.load_summary_series(path)

    def test_summary_json_loader_accepts_schema_version(self):
        data = [
            {
                "schema_version": 9,
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

        self.assertEqual(series.x_label, "time (UTC)")
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
                "schema_version": 9,
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
                "schema_version": 9,
                "interval": 1,
                "summary": {"freq": 2200.0},
            },
            {
                "schema_version": 9,
                "interval": 2,
                "summary": {"freq": None, "power": 120000.0},
            },
        ]

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            series = plot_sum.load_summary_series(path)
            series = armstat_loader.slice_summary_series(series, "2:2")

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
            armstat_loader.slice_summary_series(series, "3:4")

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
            "9,1,1774665600,2026-03-28T10:40:00+0800,SUM,2200.00,1.00\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "summary.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_sum.load_summary_series(path)

        self.assertEqual(series.x_label, "time (UTC+08:00)")
        self.assertEqual(len(series.rows), 1)
        self.assertIn("freq", series.numeric_fields)

    def test_summary_csv_loader_accepts_current_scope_and_duration(self):
        content = (
            "schema_version,interval,duration_us,timestamp,timestamp_ns,"
            "timestamp_iso,Scope,freq,busy_percent\n"
            "9,1,1000123,1774665600,1774665600123456789,"
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
            "9,1,1774665600,2026-03-28T10:40:00+0800,SUM,12.50\n"
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
            "9,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,SUM,,,120000,,,999,\n"
            "9,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,PKG,,0,,2200.00,,,\n"
            "9,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,CPU,0,,,,2200.00,,123\n"
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
            "9,1,1774665600,1774665600100000000,,SUM,,,100,,\n"
            "9,1,1774665600,1774665600100000000,,PKG,,0,,2200,\n"
            "9,1,1774665600,1774665600100000000,,CPU,0,,,,2200\n"
            "9,2,1774665600,1774665600200000000,,SUM,,,200,,\n"
            "9,2,1774665600,1774665600200000000,,PKG,,0,,2300,\n"
            "9,2,1774665600,1774665600200000000,,CPU,0,,,,2300\n"
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
                "schema_version": 9,
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
        self.assertEqual(series.x_label, "time (UTC)")
        self.assertIn("freq", series.numeric_fields)
        self.assertEqual(
            plot_cpu.resolve_field_name("freq", series.numeric_fields), "freq"
        )

    def test_cpu_csv_loader_accepts_canonical_fields(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,CPU,freq,busy_percent,temp\n"
            "9,1,1774665600,2026-03-28T10:40:00+0800,0,2200.00,1.00,45.00\n"
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "cpus.csv"
            path.write_text(content, encoding="utf-8")
            series = plot_cpu.load_cpu_series(path)

        self.assertEqual(series.cpu_ids, [0])
        self.assertEqual(series.x_label, "time (UTC+08:00)")
        self.assertIn("temp", series.numeric_fields)

    def test_cpu_csv_loader_accepts_high_lpi_usage_columns(self):
        content = (
            "schema_version,interval,timestamp,timestamp_iso,CPU,lpi4_usage\n"
            "9,1,1774665600,2026-03-28T10:40:00+0800,0,12.50\n"
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
            "9,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,SUM,,,120000,,,999,\n"
            "9,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,PKG,,0,,2200.00,,,\n"
            "9,1,1774665600,1774665600123456789,2026-03-28T10:40:00.123456789+0800,CPU,0,,,,2200.00,,123\n"
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
            "9,1,1774665600,1774665600100000000,,0,2100\n"
            "9,2,1774665600,1774665600200000000,,1,2200\n"
            "9,3,1774665600,1774665600300000000,,2,2300\n"
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
