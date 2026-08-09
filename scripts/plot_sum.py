#!/usr/bin/env python3
"""Plot summary-series data exported by armstat.

Recommended input:
    armstat -S -f json -O summary.json

The script can also read summary CSV exported with:
    armstat -S -f csv -O summary.csv

JSON carries real timestamps. CSV exports now include `timestamp` and
`timestamp_iso`, so plots can use real time directly.
"""

from __future__ import annotations

import argparse
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

_SCRIPTS_DIR = str(Path(__file__).resolve().parent)
if _SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, _SCRIPTS_DIR)

from armstat_loader import (  # noqa: E402
    SeriesData,
    load_summary_series,
    resolve_field_name,
    slice_summary_series,
)
from plot_utils import (  # noqa: E402
    smooth_series,
    finalize_figure_layout,
    load_plotting_modules,
    to_float,
)

PRESET_NAMES = ("freq", "power", "temp", "power-temp", "idle-lpi", "sysstat")


def extract_series(rows: List[Dict[str, object]], field: str) -> List[float]:
    return [to_float(row.get(field)) for row in rows]


def list_fields(series: SeriesData) -> None:
    print("Available numeric summary fields:")
    for field in series.numeric_fields:
        print(f"  {field}")


def first_available_field(available_fields: Iterable[str],
                          candidates: Iterable[str]) -> Optional[str]:
    available = set(available_fields)
    for candidate in candidates:
        if candidate in available:
            return candidate
    return None


def get_available_temp_fields(series: SeriesData) -> List[str]:
    temp_candidates = ("temp0", "temp1", "temp2", "temp3")
    return [field for field in temp_candidates if field in set(series.numeric_fields)]


def get_available_lpi_fields(series: SeriesData) -> List[str]:
    lpi_candidates = tuple(f"lpi{i}" for i in range(8))
    return [field for field in lpi_candidates if field in set(series.numeric_fields)]


def resolve_preset(series: SeriesData, preset: str) -> Tuple[List[str], List[str], str]:
    temp_fields = get_available_temp_fields(series)
    lpi_fields = get_available_lpi_fields(series)

    if preset == "freq":
        fields = ["avg_freq"]
        if "uncore_freq" in set(series.numeric_fields):
            fields.append("uncore_freq")
        return fields, [], "armstat summary: frequency"
    if preset == "power":
        return ["power"], [], "armstat summary: power"
    if preset == "temp":
        if not temp_fields:
            raise SystemExit(
                "The selected export does not contain any summary temperature field "
                "(temp0..temp3)."
            )
        return temp_fields, [], "armstat summary: temperature"
    if preset == "power-temp":
        if not temp_fields:
            raise SystemExit(
                "The selected export does not contain any summary temperature field "
                "(temp0..temp3)."
            )
        return ["power"], temp_fields, "armstat summary: power vs temperature"
    if preset == "idle-lpi":
        if not lpi_fields:
            raise SystemExit(
                "The selected export does not contain any summary idle-state field "
                "(lpi0..lpi7)."
            )
        return ["busy_percent", "idle_percent", *lpi_fields], [], (
            "armstat summary: busy/idle/lpi"
        )
    if preset == "sysstat":
        return ["ctx_switches", "interrupts", "soft_interrupts"], ["mem_bw"], (
            "armstat summary: sysstat"
        )

    raise SystemExit(f"Unknown preset '{preset}'.")


def build_output_path(input_path: Path,
                      left_fields: Sequence[str],
                      right_fields: Sequence[str],
                      preset: Optional[str] = None,
                      output_dir: Optional[Path] = None,
                      extension: str = "png") -> Path:
    if preset:
        safe = f"preset_{preset}"
    else:
        safe_parts = [field.replace(".", "_") for field in left_fields]
        if right_fields:
            safe_parts.append("vs")
            safe_parts.extend(field.replace(".", "_") for field in right_fields)
        safe = "__".join(safe_parts)
    filename = f"{input_path.stem}__{safe}.{extension}"
    if output_dir is not None:
        return output_dir / filename
    return input_path.with_name(filename)


def plot_summary(
    series: SeriesData,
    left_fields: Sequence[str],
    right_fields: Sequence[str],
    output_path: Path,
    title: Optional[str],
    smooth_window: int,
) -> None:
    plt, mdates = load_plotting_modules()

    fig, ax1 = plt.subplots(figsize=(12, 6))
    left_colors = ("#1f77b4", "#2ca02c", "#9467bd", "#8c564b")
    right_colors = ("#d62728", "#ff7f0e", "#17becf", "#bcbd22")

    ax1.set_xlabel(series.x_label)
    ax1.grid(True, linestyle="--", alpha=0.35)

    handles = []
    labels = []

    for index, field in enumerate(left_fields):
        values = smooth_series(extract_series(series.rows, field), smooth_window)
        line = ax1.plot(
            series.x_values,
            values,
            linewidth=2,
            label=field,
            color=left_colors[index % len(left_colors)],
        )[0]
        handles.append(line)
        labels.append(field)

    ax1.set_ylabel(", ".join(left_fields), color=left_colors[0])
    ax1.tick_params(axis="y", labelcolor=left_colors[0])

    if right_fields:
        ax2 = ax1.twinx()
        for index, field in enumerate(right_fields):
            values = smooth_series(extract_series(series.rows, field), smooth_window)
            line = ax2.plot(
                series.x_values,
                values,
                linewidth=2,
                label=field,
                color=right_colors[index % len(right_colors)],
            )[0]
            handles.append(line)
            labels.append(field)

        ax2.set_ylabel(", ".join(right_fields), color=right_colors[0])
        ax2.tick_params(axis="y", labelcolor=right_colors[0])

    if series.x_values and isinstance(series.x_values[0], datetime):
        locator = mdates.AutoDateLocator()
        formatter = mdates.ConciseDateFormatter(locator)
        ax1.xaxis.set_major_locator(locator)
        ax1.xaxis.set_major_formatter(formatter)

    finalize_figure_layout(fig, handles, labels, title)
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Plot armstat summary exports. JSON and current CSV exports both "
            "carry timestamps."
        )
    )
    parser.add_argument("input", help="Summary JSON or summary CSV exported by armstat")
    parser.add_argument(
        "--preset",
        choices=PRESET_NAMES,
        help=(
            "Convenience preset: freq, power, temp, power-temp, idle-lpi, "
            "or sysstat. "
            "Do not combine with --y/--y2."
        ),
    )
    parser.add_argument("--y", help="Primary summary field to plot")
    parser.add_argument("--y2", help="Optional secondary summary field for a right-side axis")
    parser.add_argument("-o", "--output", help="Output PNG path")
    parser.add_argument("--output-dir", help="Directory for auto-generated output files")
    parser.add_argument(
        "--format",
        choices=("png", "svg", "pdf"),
        default="png",
        help="Output image format used for auto-generated files or suffix-less -o paths",
    )
    parser.add_argument("--title", help="Custom plot title")
    parser.add_argument(
        "--sample-range",
        help="One-based sample range START:END to plot, for example 10:100",
    )
    parser.add_argument(
        "--smooth",
        type=int,
        default=1,
        help="Rolling-average window in samples. Default: 1 (disabled).",
    )
    parser.add_argument(
        "--list-fields",
        action="store_true",
        help="List numeric summary fields available in the input and exit",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)

    series = load_summary_series(input_path, args.sample_range)

    if input_path.suffix.lower() == ".json":
        series = slice_summary_series(series, args.sample_range)

    if args.list_fields:
        list_fields(series)
        return 0

    if args.preset and (args.y or args.y2):
        raise SystemExit("Do not combine --preset with --y/--y2.")
    if args.output and args.output_dir:
        raise SystemExit("Do not combine -o/--output with --output-dir.")
    if args.smooth <= 0:
        raise SystemExit("--smooth must be a positive integer.")

    if args.preset:
        left_fields, right_fields, default_title = resolve_preset(series, args.preset)
    else:
        if not args.y:
            raise SystemExit(
                "Specify at least one field with --y, use --preset, "
                "or use --list-fields first."
            )
        left_fields = [resolve_field_name(args.y, series.numeric_fields)]
        right_fields = [resolve_field_name(args.y2, series.numeric_fields)] if args.y2 else []
        default_title = None

    if args.output:
        output_path = Path(args.output)
        if output_path.suffix == "":
            output_path = output_path.with_suffix(f".{args.format}")
    else:
        output_dir = Path(args.output_dir) if args.output_dir else None
        output_path = build_output_path(
            input_path,
            left_fields,
            right_fields,
            args.preset,
            output_dir,
            args.format,
        )

    plot_summary(
        series,
        left_fields,
        right_fields,
        output_path,
        args.title or default_title,
        args.smooth,
    )
    print(f"wrote {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
