# SPDX-License-Identifier: GPL-2.0
"""Shared utilities for armstat plotting scripts.

This module is imported by plot_sum.py and plot_cpu.py. It contains helper
functions that are common to both summary and per-CPU plotting workflows.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


SUPPORTED_SCHEMA_VERSION = 7
SUPPORTED_SCHEMA_VERSIONS = {4, 5, 6, 7}


def load_plotting_modules():
    try:
        import matplotlib.dates as mdates
        import matplotlib.pyplot as plt
    except ModuleNotFoundError as exc:
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


def to_float(value: object) -> float:
    if value is None or value == "":
        return math.nan
    if isinstance(value, (int, float)):
        return float(value)
    return float(str(value))


def validate_schema_version(value: object, source: Path) -> None:
    if value in (None, ""):
        return

    if not is_number(value):
        raise SystemExit(f"{source} has a non-numeric schema_version field.")

    version = int(to_float(value))
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
    start = int(start_str) if start_str else 1
    end = int(end_str) if end_str else total

    if start < 1 or end < start:
        raise SystemExit("Invalid --sample-range. Expected one-based START:END.")
    if total <= 0:
        raise SystemExit("No samples available in the selected input.")

    start = min(start, total)
    end = min(end, total)
    return start, end


def smooth_series(values: Sequence[float], window: int) -> List[float]:
    if window <= 1:
        return list(values)

    smoothed: List[float] = []
    for end in range(len(values)):
        start = max(0, end - window + 1)
        bucket = [value for value in values[start:end + 1] if not math.isnan(value)]
        if not bucket:
            smoothed.append(math.nan)
        else:
            smoothed.append(sum(bucket) / len(bucket))
    return smoothed


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
