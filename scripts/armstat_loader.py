# SPDX-License-Identifier: GPL-2.0
"""Shared loader for armstat machine-readable exports.

Consolidates the JSON/CSV loaders, field-alias maps, canonicalizers, field
resolvers, series slicers, and CSV row counters that were previously
duplicated between plot_sum.py and plot_cpu.py.

Public interface:
    - FIELD_ALIASES: unified alias map (flat, scope-agnostic)
    - SeriesData / CpuSeriesData: loaded data containers
    - load_summary_series(path, sample_range) -> SeriesData
    - load_cpu_series(path, sample_range) -> CpuSeriesData
    - resolve_field_name(requested, available_fields) -> str
    - slice_summary_series / slice_cpu_series
    - count_csv_data_lines / count_csv_summary_samples / count_csv_cpu_samples
    - collect_numeric_fields (overloaded for both data shapes)
"""

from __future__ import annotations

import csv
import json
import math
import re
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

from plot_utils import (
    SUPPORTED_SCHEMA_VERSION,
    flatten_dict,
    is_number,
    normalize_field_name,
    parse_sample_range,
    to_float,
    validate_schema_version,
)


# ---------------------------------------------------------------------------
# Unified field aliases — flat dict, scope-agnostic.
#
# resolve_field_name() validates against available_fields, so a CPU-only alias
# looked up in a summary context simply won't match and raises SystemExit.
# ---------------------------------------------------------------------------

FIELD_ALIASES: Dict[str, str] = {
    # --- shared (summary + CPU) ---
    "idle": "idle_percent",
    "idle%": "idle_percent",
    "idle_percent": "idle_percent",
    "iowait": "iowait_percent",
    "iowait%": "iowait_percent",
    "iowait_percent": "iowait_percent",
    "busy": "busy_percent",
    "busy%": "busy_percent",
    "busy_percent": "busy_percent",
    "ipc": "ipc",

    # --- summary-scope ---
    "freq": "freq",
    "summary_freq_mhz": "freq",
    "uncore": "uncore_freq",
    "uncorefreq": "uncore_freq",
    "uncoremhz": "uncore_freq",
    "uncore_freq": "uncore_freq",
    "power": "power",
    "power(mw)": "power",
    "power_mw": "power",
    "energy": "energy",
    "joules": "energy",
    "energy_joules": "energy",
    "membw": "mem_bw",
    "mem_bw": "mem_bw",
    "ctxsw": "ctx_switches",
    "ctx_switches": "ctx_switches",
    "irqs": "interrupts",
    "interrupts": "interrupts",
    "softirqs": "soft_interrupts",
    "soft_interrupts": "soft_interrupts",
    "temp0": "temp0",
    "temp1": "temp1",
    "temp2": "temp2",
    "temp3": "temp3",
    "vdie0": "temp0",
    "vdie1": "temp1",
    "vdie2": "temp2",
    "vdie3": "temp3",
    "vdie0(c)": "temp0",
    "vdie1(c)": "temp1",
    "vdie2(c)": "temp2",
    "vdie3(c)": "temp3",
    "temp_vdie0": "temp0",
    "temp_vdie1": "temp1",
    "temp_vdie2": "temp2",
    "temp_vdie3": "temp3",

    # --- CPU-scope ---
    "pkg": "package",
    "package": "package",
    "core": "core",
    "node": "node",
    "numa": "node",
    "numa_node": "node",
    "cpu_idle_percent": "idle_percent",
    "cpu_iowait_percent": "iowait_percent",
    "cpu_busy_percent": "busy_percent",
    "freq_mhz": "freq",
    "temp(c)": "temp",
    "cpu_temp_c": "temp",
    "min_freq_mhz": "min",
    "max_freq_mhz": "max",
    "cycles": "pmu.cycles",
    "instructions": "pmu.instructions",
}

# Generate LPI aliases
for _idx in range(8):
    FIELD_ALIASES[f"lpi{_idx}"] = f"lpi{_idx}"
    FIELD_ALIASES[f"lpi-{_idx}"] = f"lpi{_idx}"
    FIELD_ALIASES[f"sum_idle_state{_idx}"] = f"lpi{_idx}"
    FIELD_ALIASES[f"idle_state{_idx}"] = f"lpi{_idx}"


# ---------------------------------------------------------------------------
# Data containers
# ---------------------------------------------------------------------------

@dataclass
class SeriesData:
    x_values: List[object]
    x_label: str
    rows: List[Dict[str, object]]
    numeric_fields: List[str]


@dataclass
class CpuSeriesData:
    x_values: List[object]
    x_label: str
    samples: List[Dict[int, Dict[str, object]]]
    cpu_ids: List[int]
    numeric_fields: List[str]


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

def sample_x_value(timestamp: object, sample_index: int,
                   timestamp_ns: object = None) -> object:
    if timestamp_ns not in (None, "") and is_number(timestamp_ns):
        return datetime.fromtimestamp(to_float(timestamp_ns) / 1_000_000_000.0)
    if isinstance(timestamp, (int, float)):
        return datetime.fromtimestamp(timestamp)
    if isinstance(timestamp, str) and timestamp and is_number(timestamp):
        return datetime.fromtimestamp(to_float(timestamp))
    return sample_index


def canonicalize_csv_key(key: str) -> str:
    normalized = normalize_field_name(key)
    if normalized.startswith("summary."):
        normalized = normalized.split(".", 1)[1]
    elif normalized.startswith("cpu."):
        normalized = normalized.split(".", 1)[1]
    alias_target = FIELD_ALIASES.get(normalized)
    if alias_target:
        return alias_target
    return normalized


def collect_numeric_fields(rows_or_samples: Iterable) -> List[str]:
    seen: Dict[str, None] = {}
    for item in rows_or_samples:
        if isinstance(item, dict) and all(isinstance(k, int) for k in item):
            for row in item.values():
                for key, value in row.items():
                    if key.startswith("__"):
                        continue
                    if is_number(value):
                        seen.setdefault(key, None)
        else:
            for key, value in item.items():
                if key.startswith("__"):
                    continue
                if is_number(value):
                    seen.setdefault(key, None)
    return sorted(seen.keys())


def resolve_field_name(requested: str, available_fields: Iterable[str]) -> str:
    available = list(available_fields)
    if requested in available:
        return requested

    normalized_map = {normalize_field_name(field): field for field in available}
    normalized = normalize_field_name(requested)

    if normalized in normalized_map:
        return normalized_map[normalized]

    alias_target = FIELD_ALIASES.get(normalized)
    if alias_target and alias_target in available:
        return alias_target

    match = re.fullmatch(r"lpi-(\d+)", normalized)
    if match:
        idle_field = f"lpi{match.group(1)}"
        if idle_field in available:
            return idle_field

    for field in available:
        if normalize_field_name(field.rsplit(".", 1)[-1]) == normalized:
            return field

    raise SystemExit(
        f"Unknown field '{requested}'. "
        "Use --list-fields to inspect fields available in this export."
    )


# ---------------------------------------------------------------------------
# CSV row counters
# ---------------------------------------------------------------------------

def count_csv_data_lines(path: Path) -> int:
    count = 0
    with path.open("r", encoding="utf-8") as handle:
        next(handle, None)
        for _ in handle:
            count += 1
    return count


def count_csv_summary_samples(path: Path) -> int:
    count = 0
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        is_mixed_scope = reader.fieldnames and "Scope" in reader.fieldnames
        for item in reader:
            if is_mixed_scope:
                if item.get("Scope") == "SUM":
                    count += 1
            elif item.get("SUM") == "SUM":
                count += 1
    return count


def count_csv_cpu_samples(path: Path) -> int:
    count = 0
    current_key = None
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        is_mixed_scope = reader.fieldnames and "Scope" in reader.fieldnames
        for item in reader:
            if is_mixed_scope and item.get("Scope") != "CPU":
                continue
            if not item.get("CPU"):
                continue
            key = (
                item.get("interval"), item.get("timestamp"),
                item.get("timestamp_ns"), item.get("timestamp_iso"),
            )
            if key != current_key:
                count += 1
                current_key = key
    return count


# ---------------------------------------------------------------------------
# Summary loaders
# ---------------------------------------------------------------------------

def load_json_summary(path: Path) -> SeriesData:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise SystemExit(f"{path} does not contain a JSON array.")

    rows: List[Dict[str, object]] = []
    x_values: List[object] = []

    for sample_index, item in enumerate(data, start=1):
        if not isinstance(item, dict):
            continue
        validate_schema_version(item.get("schema_version"), path)
        summary = item.get("summary")
        if not isinstance(summary, dict):
            continue

        row: Dict[str, object] = {}
        flatten_dict("", summary, row)
        row["__interval_index"] = sample_index
        row["__timestamp"] = item.get("timestamp")
        rows.append(row)

        x_values.append(sample_x_value(
            item.get("timestamp"), sample_index, item.get("timestamp_ns")
        ))

    if not rows:
        raise SystemExit(
            f"{path} does not contain summary records. "
            "Use summary JSON export, for example: armstat -S -f json -O summary.json"
        )

    x_label = "time" if isinstance(x_values[0], datetime) else "sample"
    return SeriesData(
        x_values=x_values,
        x_label=x_label,
        rows=rows,
        numeric_fields=collect_numeric_fields(rows),
    )


def load_csv_summary(path: Path, sample_range: Optional[str] = None) -> SeriesData:
    start_sample = 1
    end_sample = None

    if sample_range:
        total_samples = count_csv_summary_samples(path)
        parsed = parse_sample_range(sample_range, total_samples)
        if parsed:
            start_sample, end_sample = parsed

    rows: List[Dict[str, object]] = []
    x_values: List[object] = []

    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise SystemExit(f"{path} does not contain a CSV header.")

        is_mixed_scope = "Scope" in reader.fieldnames

        if "SUM" not in reader.fieldnames and not is_mixed_scope:
            raise SystemExit(
                f"{path} does not look like summary CSV. "
                "Use summary CSV export, for example: armstat -S -f csv -O summary.csv"
            )

        sample_index = 0
        for item in reader:
            if is_mixed_scope:
                if item.get("Scope") != "SUM":
                    continue
            elif item.get("SUM") != "SUM":
                continue
            sample_index += 1
            if sample_index < start_sample:
                continue
            if end_sample is not None and sample_index > end_sample:
                break
            validate_schema_version(item.get("schema_version"), path)

            row = {}
            for key, value in item.items():
                if key in {
                    "SUM", "Scope", "CPU", "Package",
                    "schema_version", "interval", "duration_us", "timestamp",
                    "timestamp_ns", "timestamp_iso",
                }:
                    continue
                if is_mixed_scope:
                    normalized = normalize_field_name(key)
                    if normalized.startswith(("cpu.", "package.")):
                        continue
                row[canonicalize_csv_key(key)] = value
            row["__interval_index"] = sample_index
            row["__timestamp"] = item.get("timestamp")
            rows.append(row)

            x_values.append(sample_x_value(
                item.get("timestamp"), sample_index, item.get("timestamp_ns")
            ))

    if not rows:
        raise SystemExit(f"{path} does not contain summary rows.")

    x_label = "time" if isinstance(x_values[0], datetime) else "sample"
    return SeriesData(
        x_values=x_values,
        x_label=x_label,
        rows=rows,
        numeric_fields=collect_numeric_fields(rows),
    )


def load_summary_series(path: Path, sample_range: Optional[str] = None) -> SeriesData:
    suffix = path.suffix.lower()
    if suffix == ".json":
        return load_json_summary(path)
    if suffix == ".csv":
        return load_csv_summary(path, sample_range)
    raise SystemExit(
        f"Unsupported input format for {path}. "
        "Use summary JSON or summary CSV exported by armstat."
    )


def slice_summary_series(series: SeriesData,
                         sample_range: Optional[str]) -> SeriesData:
    parsed = parse_sample_range(sample_range, len(series.rows))
    if parsed is None:
        return series
    start, end = parsed
    start_idx = start - 1
    end_idx = end
    return SeriesData(
        x_values=series.x_values[start_idx:end_idx],
        x_label=series.x_label,
        rows=series.rows[start_idx:end_idx],
        numeric_fields=series.numeric_fields,
    )


# ---------------------------------------------------------------------------
# CPU loaders
# ---------------------------------------------------------------------------

def load_json_cpu_series(path: Path) -> CpuSeriesData:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise SystemExit(f"{path} does not contain a JSON array.")

    samples: List[Dict[int, Dict[str, object]]] = []
    x_values: List[object] = []
    cpu_ids: Set[int] = set()

    for sample_index, item in enumerate(data, start=1):
        if not isinstance(item, dict):
            continue
        validate_schema_version(item.get("schema_version"), path)
        cpus = item.get("cpus")
        if not isinstance(cpus, list):
            continue

        sample: Dict[int, Dict[str, object]] = {}
        for cpu_entry in cpus:
            if not isinstance(cpu_entry, dict):
                continue
            cpu_value = cpu_entry.get("cpu")
            if cpu_value is None or not is_number(cpu_value):
                continue

            cpu_id = int(to_float(cpu_value))
            row: Dict[str, object] = {}
            for key, value in cpu_entry.items():
                if key == "cpu":
                    continue
                flatten_dict(key, value, row)

            sample[cpu_id] = row
            cpu_ids.add(cpu_id)

        if not sample:
            continue

        samples.append(sample)
        x_values.append(sample_x_value(
            item.get("timestamp"), len(samples), item.get("timestamp_ns")
        ))

    if not samples:
        raise SystemExit(
            f"{path} does not contain per-CPU JSON records. "
            "Use CPU JSON export, for example: armstat -f json -O cpus.json"
        )

    x_label = "time" if isinstance(x_values[0], datetime) else "sample"
    return CpuSeriesData(
        x_values=x_values,
        x_label=x_label,
        samples=samples,
        cpu_ids=sorted(cpu_ids),
        numeric_fields=collect_numeric_fields(samples),
    )


def load_csv_cpu_series(path: Path, sample_range: Optional[str] = None) -> CpuSeriesData:
    start_sample = 1
    end_sample = None

    if sample_range:
        total_samples = count_csv_cpu_samples(path)
        parsed = parse_sample_range(sample_range, total_samples)
        if parsed:
            start_sample, end_sample = parsed

    samples: List[Dict[int, Dict[str, object]]] = []
    x_values: List[object] = []
    cpu_ids: Set[int] = set()

    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise SystemExit(f"{path} does not contain a CSV header.")

        is_mixed_scope = "Scope" in reader.fieldnames

        if "CPU" not in reader.fieldnames:
            raise SystemExit(
                f"{path} does not look like per-CPU CSV. "
                "Use CPU CSV export, for example: armstat -f csv -O cpus.csv"
            )

        current_key: Optional[Tuple[object, object, object, object]] = None
        current_sample: Dict[int, Dict[str, object]] = {}
        sample_index = 0

        def flush_sample() -> None:
            nonlocal current_key, current_sample, sample_index
            if not current_sample or current_key is None:
                return
            sample_index += 1
            if sample_index >= start_sample and (end_sample is None or sample_index <= end_sample):
                samples.append(current_sample)
                x_values.append(sample_x_value(
                    current_key[1], sample_index, current_key[2]
                ))
            current_sample = {}

        for item in reader:
            if is_mixed_scope and item.get("Scope") != "CPU":
                continue

            cpu_value = item.get("CPU")
            if not cpu_value:
                continue
            validate_schema_version(item.get("schema_version"), path)

            key = (
                item.get("interval"), item.get("timestamp"),
                item.get("timestamp_ns"), item.get("timestamp_iso"),
            )
            if current_key is None:
                current_key = key
            elif key != current_key:
                flush_sample()
                current_key = key

            if not is_number(cpu_value):
                continue

            cpu_id = int(to_float(cpu_value))
            row = {}
            for key, value in item.items():
                if key in {
                    "Scope", "schema_version", "interval",
                    "duration_us", "timestamp", "timestamp_ns",
                    "timestamp_iso", "CPU", "Package",
                }:
                    continue
                if is_mixed_scope:
                    normalized = normalize_field_name(key)
                    if normalized.startswith(("summary.", "package.")):
                        continue
                row[canonicalize_csv_key(key)] = value
            current_sample[cpu_id] = row
            cpu_ids.add(cpu_id)

        flush_sample()

    if not samples:
        raise SystemExit(f"{path} does not contain per-CPU rows.")

    x_label = "time" if isinstance(x_values[0], datetime) else "sample"
    return CpuSeriesData(
        x_values=x_values,
        x_label=x_label,
        samples=samples,
        cpu_ids=sorted(cpu_ids),
        numeric_fields=collect_numeric_fields(samples),
    )


def load_cpu_series(path: Path, sample_range: Optional[str] = None) -> CpuSeriesData:
    suffix = path.suffix.lower()
    if suffix == ".json":
        return load_json_cpu_series(path)
    if suffix == ".csv":
        return load_csv_cpu_series(path, sample_range)
    raise SystemExit(
        f"Unsupported input format for {path}. "
        "Use CPU JSON or CPU CSV exported by armstat."
    )


def slice_cpu_series(series: CpuSeriesData,
                     sample_range: Optional[str]) -> CpuSeriesData:
    parsed = parse_sample_range(sample_range, len(series.samples))
    if parsed is None:
        return series
    start, end = parsed
    start_idx = start - 1
    end_idx = end
    sliced_samples = series.samples[start_idx:end_idx]
    cpu_ids: Set[int] = set()
    for sample in sliced_samples:
        cpu_ids.update(sample.keys())
    return CpuSeriesData(
        x_values=series.x_values[start_idx:end_idx],
        x_label=series.x_label,
        samples=sliced_samples,
        cpu_ids=sorted(cpu_ids),
        numeric_fields=collect_numeric_fields(sliced_samples),
    )
