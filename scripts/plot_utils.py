# SPDX-License-Identifier: GPL-2.0
"""Shared utilities for armstat plotting scripts.

This module is imported by plot_sum.py and plot_cpu.py. It contains helper
functions that are common to both summary and per-CPU plotting workflows.
"""

from __future__ import annotations

import math
import os
import re
import tempfile
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


SUPPORTED_SCHEMA_VERSION = 8
SUPPORTED_SCHEMA_VERSIONS = {SUPPORTED_SCHEMA_VERSION}
SUPPORTED_PLOT_SUFFIXES = {".pdf", ".png", ".svg"}

FIELD_UNITS = {
    "freq": "MHz",
    "uncore_freq": "MHz",
    "min": "MHz",
    "max": "MHz",
    "idle_percent": "%",
    "iowait_percent": "%",
    "busy_percent": "%",
    "power": "mW",
    "energy": "J",
    "mem_bw": "MiB/s",
    "ctx_switches": "count/interval",
    "interrupts": "count/interval",
    "soft_interrupts": "count/interval",
    "ipc": "instructions/cycle",
    "temp": "degC",
    "temp0": "degC",
    "temp1": "degC",
    "temp2": "degC",
    "temp3": "degC",
}


def load_plotting_modules():
    try:
        import matplotlib

        # These helpers always render to a file.  A non-interactive backend
        # keeps them usable on headless servers regardless of DISPLAY state.
        matplotlib.use("Agg")
        import matplotlib.dates as mdates
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise SystemExit(
            "matplotlib is required for plotting. Install it with:\n"
            "  python3 -m pip install matplotlib"
        ) from exc

    return plt, mdates


def flatten_dict(prefix: str, value: object, out: Dict[str, object]) -> None:
    if isinstance(value, dict):
        for key, nested in value.items():
            name = f"{prefix}.{key}" if prefix else key
            flatten_dict(name, nested, out)
        return

    out[prefix] = value


def normalize_field_name(name: str) -> str:
    return name.strip().lower()


def is_number(value: object) -> bool:
    if isinstance(value, bool):
        return False
    if isinstance(value, (int, float)):
        return True
    if not isinstance(value, str):
        return False
    try:
        float(value)
        return True
    except ValueError:
        return False


def is_finite_number(value: object) -> bool:
    return is_number(value) and math.isfinite(to_float(value))


def to_float(value: object) -> float:
    if value is None or value == "":
        return math.nan
    try:
        if isinstance(value, (int, float)):
            numeric = float(value)
        else:
            numeric = float(str(value))
    except (TypeError, ValueError):
        return math.nan
    return numeric if math.isfinite(numeric) else math.nan


def field_unit(field: str) -> Optional[str]:
    if field in FIELD_UNITS:
        return FIELD_UNITS[field]
    if re.fullmatch(r"lpi\d+", field):
        return "%"
    if re.fullmatch(r"lpi\d+_usage", field):
        return "/s"
    if field.startswith("pmu."):
        return "count/interval"
    return None


def field_axis_label(fields: Sequence[str]) -> str:
    resolved_units = [field_unit(field) for field in fields]
    names = ", ".join(fields)
    if resolved_units and resolved_units[0] is not None and all(
        unit == resolved_units[0] for unit in resolved_units
    ):
        return f"{names} ({resolved_units[0]})"
    if not any(resolved_units):
        return names

    labels: List[str] = []
    for field, unit in zip(fields, resolved_units):
        labels.append(f"{field} ({unit})" if unit else field)
    return ", ".join(labels)


def field_list_label(field: str) -> str:
    unit = field_unit(field)
    return f"{field} [{unit}]" if unit else field


def validate_schema_version(value: object, source: Path) -> None:
    if value in (None, ""):
        raise SystemExit(f"{source} is missing the required schema_version field.")

    if not is_number(value):
        raise SystemExit(f"{source} has a non-numeric schema_version field.")

    numeric_version = to_float(value)
    if not math.isfinite(numeric_version) or not numeric_version.is_integer():
        raise SystemExit(
            f"{source} has an invalid schema_version={value!r}; "
            "expected an integer."
        )

    version = int(numeric_version)
    if version not in SUPPORTED_SCHEMA_VERSIONS:
        raise SystemExit(
            f"{source} uses schema_version={version}, "
            f"but this script supports {sorted(SUPPORTED_SCHEMA_VERSIONS)}."
        )


def parse_sample_range(expr: Optional[str], total: int) -> Optional[Tuple[int, int]]:
    if not expr:
        return None

    sep = ":" if ":" in expr else "-"
    if sep not in expr:
        raise SystemExit("Use --sample-range START:END, for example 10:100.")

    start_str, end_str = expr.split(sep, 1)
    try:
        start = int(start_str) if start_str else 1
        end = int(end_str) if end_str else total
    except ValueError as exc:
        raise SystemExit(
            "Invalid --sample-range. Expected one-based START:END."
        ) from exc

    if start < 1 or end < start:
        raise SystemExit("Invalid --sample-range. Expected one-based START:END.")
    if total <= 0:
        raise SystemExit("No samples available in the selected input.")
    if start > total:
        raise SystemExit(
            f"--sample-range starts at {start}, but the input contains only "
            f"{total} samples."
        )

    end = min(end, total)
    return start, end


def smooth_series(values: Sequence[float], window: int) -> List[float]:
    finite_values = [
        value if math.isfinite(value) else math.nan
        for value in values
    ]
    if window <= 1:
        return finite_values

    smoothed: List[float] = []
    for end in range(len(finite_values)):
        if not math.isfinite(finite_values[end]):
            smoothed.append(math.nan)
            continue
        start = max(0, end - window + 1)
        bucket = [
            value for value in finite_values[start:end + 1]
            if math.isfinite(value)
        ]
        if not bucket:
            smoothed.append(math.nan)
        else:
            smoothed.append(sum(bucket) / len(bucket))
    return smoothed


def configure_time_axis(axis, x_values: Sequence[object], mdates) -> None:
    if not x_values or not isinstance(x_values[0], datetime):
        return

    timezone = x_values[0].tzinfo
    locator = mdates.AutoDateLocator(tz=timezone)
    formatter = mdates.ConciseDateFormatter(locator, tz=timezone)
    axis.xaxis.set_major_locator(locator)
    axis.xaxis.set_major_formatter(formatter)


def save_figure(fig, output_path: Path) -> None:
    suffix = output_path.suffix.lower()
    if suffix not in SUPPORTED_PLOT_SUFFIXES:
        supported = ", ".join(sorted(item[1:] for item in SUPPORTED_PLOT_SUFFIXES))
        raise SystemExit(
            f"Unsupported plot output format '{output_path.suffix or '(none)'}'. "
            f"Use one of: {supported}."
        )

    temporary_path: Optional[Path] = None
    try:
        fd, name = tempfile.mkstemp(
            prefix=f".{output_path.stem}.",
            suffix=suffix,
            dir=output_path.parent,
        )
        os.close(fd)
        temporary_path = Path(name)
        fig.savefig(
            temporary_path,
            dpi=160,
            bbox_inches="tight",
            format=suffix[1:],
        )
        os.replace(temporary_path, output_path)
        temporary_path = None
    except Exception as exc:
        raise SystemExit(f"Could not write plot {output_path}: {exc}") from exc
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass


def compute_legend_columns(label_count: int) -> int:
    if label_count <= 4:
        return label_count
    if label_count <= 8:
        return 2
    if label_count <= 12:
        return 3
    return 4


def legend_font_size(label_count: int) -> str:
    if label_count <= 6:
        return "medium"
    if label_count <= 12:
        return "small"
    return "x-small"


def finalize_figure_layout(fig,
                           handles,
                           labels: Sequence[str],
                           title: Optional[str] = None) -> None:
    if title:
        fig.suptitle(title, y=0.99)

    if handles and labels:
        if len(labels) > 12:
            fig.legend(
                handles,
                labels,
                loc="center left",
                bbox_to_anchor=(1.01, 0.5),
                ncol=1,
                frameon=False,
                fontsize=legend_font_size(len(labels)),
                handlelength=2.2,
                labelspacing=0.6,
                borderaxespad=0.0,
            )
            fig.tight_layout(rect=(0, 0, 0.76, 0.94))
            return

        legend_cols = compute_legend_columns(len(labels))
        legend_rows = math.ceil(len(labels) / legend_cols)
        fig.legend(
            handles,
            labels,
            loc="upper center",
            bbox_to_anchor=(0.5, 0.948),
            ncol=legend_cols,
            frameon=False,
            fontsize=legend_font_size(len(labels)),
            handlelength=2.2,
            columnspacing=1.4,
            labelspacing=0.6,
            borderaxespad=0.0,
        )
        top_reserved = 0.90 - (0.065 * legend_rows)
        fig.tight_layout(rect=(0, 0, 1, max(0.56, top_reserved)))
        return

    fig.tight_layout(rect=(0, 0, 1, 0.94))
