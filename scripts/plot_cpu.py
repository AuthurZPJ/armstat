#!/usr/bin/env python3
"""Plot per-CPU time-series data exported by armstat.

Recommended input:
    armstat -f json -O cpus.json

The script can also read per-CPU CSV exported with:
    armstat -f csv -O cpus.csv

JSON carries real timestamps. CSV exports now include `timestamp` and
`timestamp_iso`, so plots can use real time directly.
"""

from __future__ import annotations

import argparse
import math
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

_SCRIPTS_DIR = str(Path(__file__).resolve().parent)
if _SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, _SCRIPTS_DIR)

from armstat_loader import (  # noqa: E402
    CpuSeriesData,
    load_cpu_series,
    resolve_field_name,
    slice_cpu_series,
)
from plot_utils import (  # noqa: E402
    is_number,
    smooth_series,
    to_float,
    finalize_figure_layout,
    load_plotting_modules,
)

PRESET_NAMES = ("freq", "temp", "idle", "busy", "iowait", "ipc")
DEFAULT_CPU_PLOT_LIMIT = 8
GROUP_BY_CHOICES = ("node", "core")
AGGREGATE_CHOICES = ("avg", "max", "min")


def resolve_preset(series: CpuSeriesData, preset: str) -> Tuple[str, Optional[str], str]:
    mapping = {
        "freq": ("freq", None, "armstat CPUs: frequency"),
        "temp": ("temp", None, "armstat CPUs: temperature"),
        "idle": ("idle_percent", None, "armstat CPUs: idle"),
        "busy": ("busy_percent", None, "armstat CPUs: busy"),
        "iowait": ("iowait_percent", None, "armstat CPUs: iowait"),
        "ipc": ("ipc", None, "armstat CPUs: IPC"),
    }
    field1, field2, title = mapping[preset]
    left = resolve_field_name(field1, series.numeric_fields)
    right = resolve_field_name(field2, series.numeric_fields) if field2 else None
    return left, right, title


def parse_cpu_list(expr: Optional[str]) -> Optional[List[int]]:
    if not expr:
        return None

    selected: Set[int] = set()
    for token in expr.split(","):
        part = token.strip()
        if not part:
            continue
        if "-" in part:
            start_str, end_str = part.split("-", 1)
            start = int(start_str)
            end = int(end_str)
            if end < start:
                start, end = end, start
            for cpu in range(start, end + 1):
                selected.add(cpu)
        else:
            selected.add(int(part))

    return sorted(selected)


def select_cpu_ids(series: CpuSeriesData, cpu_filter: Optional[str]) -> List[int]:
    requested = parse_cpu_list(cpu_filter)
    if requested is None:
        return series.cpu_ids

    available = set(series.cpu_ids)
    selected = [cpu for cpu in requested if cpu in available]
    if not selected:
        raise SystemExit(
            "The requested CPU filter does not match any CPU in the export. "
            "Use --list-cpus to inspect available CPU IDs."
        )
    return selected


def sort_entity_keys(keys: Iterable[object]) -> List[object]:
    def sort_key(value: object) -> Tuple[int, object]:
        if isinstance(value, (int, float)):
            return (0, value)
        if isinstance(value, str) and value and is_number(value):
            return (0, to_float(value))
        return (1, str(value))

    return sorted(keys, key=sort_key)


def get_cpu_group_value(series: CpuSeriesData,
                        cpu_id: int,
                        group_field: str) -> Optional[object]:
    for sample in series.samples:
        row = sample.get(cpu_id)
        if not row:
            continue
        value = row.get(group_field)
        if value is None or value == "":
            continue
        if isinstance(value, str) and is_number(value):
            numeric = to_float(value)
            if float(numeric).is_integer():
                return int(numeric)
            return numeric
        return value
    return None


def build_cpu_groups(series: CpuSeriesData,
                     cpu_ids: Sequence[int],
                     group_by: Optional[str]) -> Tuple[Dict[object, List[int]], Optional[str], Optional[str]]:
    if group_by is None:
        return {}, None, None

    if group_by == "node":
        group_field = "node"
        group_label = "node"
    elif group_by == "core":
        group_field = "core"
        group_label = "core"
    else:
        raise SystemExit(f"Unsupported --group-by value '{group_by}'.")

    groups: Dict[object, List[int]] = {}
    for cpu_id in cpu_ids:
        group_value = get_cpu_group_value(series, cpu_id, group_field)
        if group_value is None:
            continue
        groups.setdefault(group_value, []).append(cpu_id)

    if not groups:
        raise SystemExit(
            f"Could not derive any CPU groups for --group-by {group_by}. "
            "Check that the export contains the required topology fields."
        )

    return groups, group_field, group_label


def extract_series(samples: Sequence[Dict[int, Dict[str, object]]],
                   cpu_id: int,
                   field: str) -> List[float]:
    values: List[float] = []
    for sample in samples:
        row = sample.get(cpu_id, {})
        values.append(to_float(row.get(field)))
    return values


def average_field_value(series: CpuSeriesData, cpu_id: int, field: str) -> float:
    values = [value for value in extract_series(series.samples, cpu_id, field)
              if not math.isnan(value)]
    if not values:
        return float("-inf")
    return sum(values) / len(values)


def max_field_value(series: CpuSeriesData, cpu_id: int, field: str) -> float:
    values = [value for value in extract_series(series.samples, cpu_id, field)
              if not math.isnan(value)]
    if not values:
        return float("-inf")
    return max(values)


def last_field_value(series: CpuSeriesData, cpu_id: int, field: str) -> float:
    for value in reversed(extract_series(series.samples, cpu_id, field)):
        if not math.isnan(value):
            return value
    return float("-inf")


def apply_top_selection(series: CpuSeriesData,
                        cpu_ids: Sequence[int],
                        field: str,
                        top_n: Optional[int],
                        rank_by: str) -> List[int]:
    if top_n is None:
        return list(cpu_ids)
    if top_n <= 0:
        raise SystemExit("--top must be a positive integer.")
    if top_n >= len(cpu_ids):
        return list(cpu_ids)

    rankers = {
        "avg": average_field_value,
        "max": max_field_value,
        "last": last_field_value,
    }
    rank_fn = rankers[rank_by]

    ranked = sorted(
        cpu_ids,
        key=lambda cpu_id: (rank_fn(series, cpu_id, field), -cpu_id),
        reverse=True,
    )
    selected = ranked[:top_n]
    return sorted(selected)


def aggregate_group_series(samples: Sequence[Dict[int, Dict[str, object]]],
                           cpu_ids: Sequence[int],
                           field: str,
                           aggregate_mode: str) -> List[float]:
    values: List[float] = []
    for sample in samples:
        bucket: List[float] = []
        for cpu_id in cpu_ids:
            row = sample.get(cpu_id, {})
            value = to_float(row.get(field))
            if not math.isnan(value):
                bucket.append(value)
        if not bucket:
            values.append(math.nan)
        else:
            if aggregate_mode == "avg":
                values.append(sum(bucket) / len(bucket))
            elif aggregate_mode == "max":
                values.append(max(bucket))
            elif aggregate_mode == "min":
                values.append(min(bucket))
            else:
                raise SystemExit(f"Unsupported --aggregate value '{aggregate_mode}'.")
    return values


def aggregate_group_value(series: CpuSeriesData,
                          cpu_ids: Sequence[int],
                          field: str,
                          rank_by: str,
                          aggregate_mode: str) -> float:
    values = [value for value in aggregate_group_series(
        series.samples, cpu_ids, field, aggregate_mode)
              if not math.isnan(value)]
    if not values:
        return float("-inf")
    if rank_by == "avg":
        return sum(values) / len(values)
    if rank_by == "max":
        return max(values)
    if rank_by == "last":
        return values[-1]
    raise SystemExit(f"Unsupported ranking mode '{rank_by}'.")


def apply_top_group_selection(series: CpuSeriesData,
                              groups: Dict[object, List[int]],
                              field: str,
                              top_n: Optional[int],
                              rank_by: str,
                              aggregate_mode: str) -> Dict[object, List[int]]:
    if top_n is None:
        return groups
    if top_n <= 0:
        raise SystemExit("--top must be a positive integer.")
    if top_n >= len(groups):
        return groups

    ranked = sorted(
        groups.items(),
        key=lambda item: (
            aggregate_group_value(series, item[1], field, rank_by, aggregate_mode),
            str(item[0]),
        ),
        reverse=True,
    )
    selected = ranked[:top_n]
    return dict(sorted(selected, key=lambda item: sort_entity_keys([item[0]])[0]))


def build_output_path(input_path: Path,
                      cpu_ids: Sequence[int],
                      left_field: str,
                      right_field: Optional[str],
                      preset: Optional[str],
                      top_n: Optional[int],
                      group_by: Optional[str],
                      output_dir: Optional[Path],
                      extension: str) -> Path:
    if group_by is not None:
        cpu_label = f"{group_by}_{len(cpu_ids)}groups"
    else:
        cpu_label = (
            f"cpu{cpu_ids[0]}"
            if len(cpu_ids) == 1
            else f"cpu{cpu_ids[0]}-{cpu_ids[-1]}_{len(cpu_ids)}cpus"
        )
    if top_n is not None:
        cpu_label = f"top{len(cpu_ids)}_{cpu_label}"
    metric_label = f"preset_{preset}" if preset else left_field.replace(".", "_")
    if right_field:
        metric_label = f"{metric_label}__vs__{right_field.replace('.', '_')}"
    filename = f"{input_path.stem}__{cpu_label}__{metric_label}.{extension}"
    if output_dir is not None:
        return output_dir / filename
    return input_path.with_name(filename)


def list_fields(series: CpuSeriesData) -> None:
    print("Available numeric CPU fields:")
    for field in series.numeric_fields:
        print(f"  {field}")


def list_cpus(series: CpuSeriesData) -> None:
    print("Available CPU IDs:")
    print("  " + ",".join(str(cpu) for cpu in series.cpu_ids))


def format_group_name(group_label: str, group_value: object) -> str:
    return f"{group_label}{group_value}"


def plot_cpu_series(series: CpuSeriesData,
                    cpu_ids: Sequence[int],
                    left_field: str,
                    right_field: Optional[str],
                    output_path: Path,
                    title: Optional[str],
                    smooth_window: int) -> None:
    plt, mdates = load_plotting_modules()

    fig, ax1 = plt.subplots(figsize=(12, 6))
    ax1.set_xlabel(series.x_label)
    ax1.grid(True, linestyle="--", alpha=0.35)

    color_map = plt.get_cmap("tab20")
    handles = []
    labels = []

    for index, cpu_id in enumerate(cpu_ids):
        color = color_map(index % 20)
        values = smooth_series(extract_series(series.samples, cpu_id, left_field),
                               smooth_window)
        line = ax1.plot(
            series.x_values,
            values,
            linewidth=1.8,
            color=color,
            label=f"cpu{cpu_id}:{left_field}",
        )[0]
        handles.append(line)
        labels.append(f"cpu{cpu_id}:{left_field}")

    ax1.set_ylabel(left_field)

    if right_field:
        ax2 = ax1.twinx()
        for index, cpu_id in enumerate(cpu_ids):
            color = color_map(index % 20)
            values = smooth_series(extract_series(series.samples, cpu_id, right_field),
                                   smooth_window)
            line = ax2.plot(
                series.x_values,
                values,
                linewidth=1.6,
                linestyle="--",
                color=color,
                label=f"cpu{cpu_id}:{right_field}",
            )[0]
            handles.append(line)
            labels.append(f"cpu{cpu_id}:{right_field}")
        ax2.set_ylabel(right_field)

    if series.x_values and isinstance(series.x_values[0], datetime):
        locator = mdates.AutoDateLocator()
        formatter = mdates.ConciseDateFormatter(locator)
        ax1.xaxis.set_major_locator(locator)
        ax1.xaxis.set_major_formatter(formatter)

    finalize_figure_layout(fig, handles, labels, title or "armstat CPUs")
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def plot_group_series(series: CpuSeriesData,
                      groups: Dict[object, List[int]],
                      group_label: str,
                      left_field: str,
                      right_field: Optional[str],
                      output_path: Path,
                      title: Optional[str],
                      smooth_window: int,
                      aggregate_mode: str) -> None:
    plt, mdates = load_plotting_modules()

    fig, ax1 = plt.subplots(figsize=(12, 6))
    ax1.set_xlabel(series.x_label)
    ax1.grid(True, linestyle="--", alpha=0.35)

    color_map = plt.get_cmap("tab20")
    handles = []
    labels = []

    ordered_group_values = sort_entity_keys(groups.keys())
    for index, group_value in enumerate(ordered_group_values):
        color = color_map(index % 20)
        values = smooth_series(
            aggregate_group_series(
                series.samples, groups[group_value], left_field, aggregate_mode),
            smooth_window,
        )
        label = f"{format_group_name(group_label, group_value)}:{left_field}"
        line = ax1.plot(
            series.x_values,
            values,
            linewidth=1.8,
            color=color,
            label=label,
        )[0]
        handles.append(line)
        labels.append(label)

    ax1.set_ylabel(left_field)

    if right_field:
        ax2 = ax1.twinx()
        for index, group_value in enumerate(ordered_group_values):
            color = color_map(index % 20)
            values = smooth_series(
                aggregate_group_series(
                    series.samples, groups[group_value], right_field, aggregate_mode),
                smooth_window,
            )
            label = f"{format_group_name(group_label, group_value)}:{right_field}"
            line = ax2.plot(
                series.x_values,
                values,
                linewidth=1.6,
                linestyle="--",
                color=color,
                label=label,
            )[0]
            handles.append(line)
            labels.append(label)
        ax2.set_ylabel(right_field)

    if series.x_values and isinstance(series.x_values[0], datetime):
        locator = mdates.AutoDateLocator()
        formatter = mdates.ConciseDateFormatter(locator)
        ax1.xaxis.set_major_locator(locator)
        ax1.xaxis.set_major_formatter(formatter)

    finalize_figure_layout(
        fig,
        handles,
        labels,
        title or f"armstat CPUs grouped by {group_label}",
    )
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Plot armstat per-CPU exports. JSON and current CSV exports both "
            "carry timestamps. "
            "On large exports, use --cpu-filter to keep plots readable."
        )
    )
    parser.add_argument("input", help="CPU JSON or CPU CSV exported by armstat")
    parser.add_argument(
        "--preset",
        choices=PRESET_NAMES,
        help=(
            "Convenience preset: freq, temp, idle, busy, iowait, or ipc. "
            "Do not combine with --y/--y2."
        ),
    )
    parser.add_argument(
        "--cpu-filter",
        help=(
            "CPU list such as 0,1,4-7. Strongly recommended on large systems; "
            "the script will not plot every CPU by default when many CPUs are present."
        ),
    )
    parser.add_argument(
        "--group-by",
        choices=GROUP_BY_CHOICES,
        help=(
            "Aggregate CPUs into lines grouped by node or core. Grouped plots "
            "use the selected --aggregate mode within each group."
        ),
    )
    parser.add_argument(
        "--aggregate",
        choices=AGGREGATE_CHOICES,
        default="avg",
        help=(
            "When --group-by is used, aggregate CPUs within each group using "
            "avg, max, or min. Default: avg."
        ),
    )
    parser.add_argument(
        "--top",
        type=int,
        help=(
            "Select the top N CPUs ranked by the average value of the primary field "
            "over the plotted time range. Applied after --cpu-filter, and after "
            "grouping when --group-by is used."
        ),
    )
    parser.add_argument(
        "--rank-by",
        choices=("avg", "max", "last"),
        default="avg",
        help=(
            "Ranking rule for --top. "
            "'avg' uses the time-average, 'max' uses the peak value, and "
            "'last' uses the last visible sample. Default: avg."
        ),
    )
    parser.add_argument("--y", help="Primary CPU field to plot")
    parser.add_argument("--y2", help="Optional secondary CPU field for a right-side axis")
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
        help="List numeric CPU fields available in the input and exit",
    )
    parser.add_argument(
        "--list-cpus",
        action="store_true",
        help="List CPU IDs available in the input and exit",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    input_path = Path(args.input)

    series = load_cpu_series(input_path, args.sample_range)

    if input_path.suffix.lower() == ".json":
        series = slice_cpu_series(series, args.sample_range)

    if args.list_fields:
        list_fields(series)
        return 0

    if args.list_cpus:
        list_cpus(series)
        return 0

    if args.preset and (args.y or args.y2):
        raise SystemExit("Do not combine --preset with --y/--y2.")
    if args.output and args.output_dir:
        raise SystemExit("Do not combine -o/--output with --output-dir.")
    if args.smooth <= 0:
        raise SystemExit("--smooth must be a positive integer.")
    if args.group_by is None and args.aggregate != "avg":
        raise SystemExit("--aggregate is only meaningful together with --group-by.")

    if args.preset:
        left_field, right_field, default_title = resolve_preset(series, args.preset)
    else:
        if not args.y:
            raise SystemExit(
                "Specify at least one field with --y, use --preset, "
                "or use --list-fields first."
            )
        left_field = resolve_field_name(args.y, series.numeric_fields)
        right_field = resolve_field_name(args.y2, series.numeric_fields) if args.y2 else None
        default_title = None

    cpu_ids = select_cpu_ids(series, args.cpu_filter)
    groups, group_field, group_label = build_cpu_groups(series, cpu_ids, args.group_by)

    if group_label is None:
        cpu_ids = apply_top_selection(series, cpu_ids, left_field, args.top, args.rank_by)
        if args.top is None and len(cpu_ids) > DEFAULT_CPU_PLOT_LIMIT:
            raise SystemExit(
                f"The export contains {len(cpu_ids)} CPUs. Please narrow the plot "
                "with --cpu-filter or --top."
            )
    else:
        groups = apply_top_group_selection(
            series, groups, left_field, args.top, args.rank_by, args.aggregate)
        if args.top is None and len(groups) > DEFAULT_CPU_PLOT_LIMIT:
            raise SystemExit(
                f"The grouped plot contains {len(groups)} groups. Please narrow the "
                "plot with --cpu-filter or --top."
            )

    if args.output:
        output_path = Path(args.output)
        if output_path.suffix == "":
            output_path = output_path.with_suffix(f".{args.format}")
    else:
        output_dir = Path(args.output_dir) if args.output_dir else None
        output_path = build_output_path(
            input_path,
            sort_entity_keys(groups.keys()) if group_label is not None else cpu_ids,
            left_field,
            right_field,
            args.preset,
            args.top,
            args.group_by,
            output_dir,
            args.format,
        )

    if group_label is None:
        plot_cpu_series(
            series,
            cpu_ids,
            left_field,
            right_field,
            output_path,
            args.title or default_title,
            args.smooth,
        )
    else:
        plot_group_series(
            series,
            groups,
            group_label,
            left_field,
            right_field,
            output_path,
            args.title or default_title,
            args.smooth,
            args.aggregate,
        )
    print(f"wrote {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
