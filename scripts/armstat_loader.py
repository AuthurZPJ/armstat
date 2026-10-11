# SPDX-License-Identifier: GPL-2.0
"""Shared loader for armstat machine-readable exports.

Consolidates the JSON/NDJSON/CSV loaders, field-alias maps, canonicalizers, field
resolvers, series slicers, and CSV row counters that were previously
duplicated between plot_sum.py and plot_cpu.py.

Public interface:
    - FIELD_ALIASES: unified alias map (flat, scope-agnostic)
    - SeriesData / CpuSeriesData: loaded data containers
    - load_summary_series(path, sample_range, fields=None) -> SeriesData
    - load_cpu_series(path, sample_range, cpu_filter=None, fields=None) -> CpuSeriesData
    - resolve_field_name(requested, available_fields) -> str
    - slice_summary_series / slice_cpu_series
    - count_csv_summary_samples / count_csv_cpu_samples
    - collect_numeric_fields (overloaded for both data shapes)
"""

from __future__ import annotations

import csv
import json
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Optional, Set, Tuple

from plot_utils import (
    flatten_dict,
    is_finite_number,
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
    FIELD_ALIASES[f"lpi{_idx}_usage"] = f"lpi{_idx}_usage"
    FIELD_ALIASES[f"lpi-{_idx}_usage"] = f"lpi{_idx}_usage"
    FIELD_ALIASES[f"idle_state_usage{_idx}"] = f"lpi{_idx}_usage"


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

class DuplicateJsonKeyError(ValueError):
    def __init__(self, key: str):
        super().__init__(key)
        self.key = key


class InvalidJsonConstantError(ValueError):
    def __init__(self, value: str):
        super().__init__(value)
        self.value = value


def reject_duplicate_json_keys(
    pairs: Iterable[Tuple[str, object]],
) -> Dict[str, object]:
    result: Dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJsonKeyError(key)
        result[key] = value
    return result


def reject_nonstandard_json_constant(value: str):
    raise InvalidJsonConstantError(value)


def is_incomplete_json_object(text: str) -> bool:
    """Recognize a valid object prefix, without accepting earlier damage.

    Used only for explicit EOF recovery. The standard decoder's error position
    alone cannot distinguish a truncated number from an invalid token, or find
    duplicate keys in an object whose closing brace was never written.
    """
    position = 0
    length = len(text)
    decoder = json.JSONDecoder()

    class Incomplete(Exception):
        pass

    def peek() -> str:
        nonlocal position
        while position < length and text[position] in " \t\r\n":
            position += 1
        if position == length:
            raise Incomplete
        return text[position]

    def string() -> str:
        nonlocal position
        start = position
        position += 1
        while position < length:
            char = text[position]
            position += 1
            if char == '"':
                return decoder.decode(text[start:position])
            if ord(char) < 0x20:
                raise ValueError("Invalid string control character")
            if char == "\\":
                if position == length:
                    raise Incomplete
                escape = text[position]
                position += 1
                if escape == "u":
                    for _ in range(4):
                        if position == length:
                            raise Incomplete
                        if text[position] not in "0123456789abcdefABCDEF":
                            raise ValueError("Invalid unicode escape")
                        position += 1
                elif escape not in '\\"/bfnrt':
                    raise ValueError("Invalid string escape")
        raise Incomplete

    def value() -> None:
        nonlocal position
        char = peek()
        if char == '"':
            string()
        elif char in "{[":
            is_object = char == "{"
            close = "}" if is_object else "]"
            keys: Set[str] = set()
            position += 1
            if peek() == close:
                position += 1
                return
            while True:
                if is_object:
                    if peek() != '"':
                        raise ValueError("Expected object key")
                    key = string()
                    if key in keys:
                        raise DuplicateJsonKeyError(key)
                    keys.add(key)
                    if peek() != ":":
                        raise ValueError("Expected colon")
                    position += 1
                value()
                separator = peek()
                position += 1
                if separator == close:
                    return
                if separator != ",":
                    raise ValueError("Expected separator")
                # A comma requires another element, even before a closing brace.
                if peek() == close:
                    raise ValueError("Trailing comma")
        elif char in "tfn":
            literal = {"t": "true", "f": "false", "n": "null"}[char]
            for expected in literal:
                if position == length:
                    raise Incomplete
                if text[position] != expected:
                    raise ValueError("Invalid JSON literal")
                position += 1
        elif char in "-0123456789":
            start = position
            while position < length and text[position] not in " \t\r\n,]}":
                position += 1
            token = text[start:position]
            if re.fullmatch(r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?", token):
                decoder.decode(token)
            elif position == length and (
                token == "-" or
                re.fullmatch(r"-?(?:0|[1-9][0-9]*)(?:\.|(?:\.[0-9]+)?[eE][+-]?)", token)
            ):
                raise Incomplete
            else:
                raise ValueError("Invalid JSON number")
        else:
            raise ValueError("Invalid JSON value")

    try:
        if peek() != "{":
            return False
        value()
    except Incomplete:
        return True
    except (ValueError, RecursionError):
        return False
    return False


def iter_ndjson_samples(path: Path, recover_tail: bool = False) -> Iterator[object]:
    """Read one strictly validated JSON object per physical line."""
    with path.open("rb") as handle:
        for line_number, raw in enumerate(handle, start=1):
            terminated = raw.endswith(b"\n")
            try:
                text = raw.decode("utf-8")
            except UnicodeDecodeError as exc:
                if (recover_tail and not terminated and
                        exc.reason == "unexpected end of data" and
                        is_incomplete_json_object(raw[:exc.start].decode("utf-8"))):
                    print(f"{path}: discarded 1 incomplete NDJSON tail record "
                          f"at line {line_number}.", file=sys.stderr)
                    return
                raise
            try:
                item = json.loads(
                    text, object_pairs_hook=reject_duplicate_json_keys,
                    parse_constant=reject_nonstandard_json_constant,
                )
            except DuplicateJsonKeyError as exc:
                raise SystemExit(f"{path} line {line_number} contains duplicate JSON key {exc.key!r}.") from exc
            except InvalidJsonConstantError as exc:
                raise SystemExit(f"{path} line {line_number} contains non-standard JSON constant {exc.value!r}.") from exc
            except json.JSONDecodeError as exc:
                if recover_tail and not terminated and is_incomplete_json_object(text):
                    print(f"{path}: discarded 1 incomplete NDJSON tail record "
                          f"at line {line_number}.", file=sys.stderr)
                    return
                raise SystemExit(f"{path} line {line_number} contains invalid NDJSON: "
                                 f"{exc.msg} (column {exc.colno}).") from exc
            if not isinstance(item, dict):
                raise SystemExit(f"{path} line {line_number} is not a JSON object.")
            yield item


def detect_export_format(path: Path) -> str:
    """Prefer file contents; extensions also identify damaged/empty inputs."""
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(64 * 1024)
            if not chunk:
                break
            prefix = chunk.lstrip(b" \t\r\n")
            if not prefix:
                continue
            if prefix[:1] == b"[":
                return "json"
            if prefix[:1] == b"{":
                return "ndjson"
            break
    suffix = path.suffix.lower()
    if suffix in {".json", ".csv", ".ndjson", ".jsonl"}:
        return "ndjson" if suffix in {".ndjson", ".jsonl"} else suffix[1:]
    raise SystemExit(f"Unsupported input format for {path}. Use armstat JSON, NDJSON, or CSV exports.")


def decode_json_sample(text: str, path: Path) -> object:
    try:
        return json.loads(
            text,
            object_pairs_hook=reject_duplicate_json_keys,
            parse_constant=reject_nonstandard_json_constant,
        )
    except DuplicateJsonKeyError as exc:
        raise SystemExit(
            f"{path} contains duplicate JSON key {exc.key!r}."
        ) from exc
    except InvalidJsonConstantError as exc:
        raise SystemExit(
            f"{path} contains non-standard JSON constant {exc.value!r}."
        ) from exc


def iter_json_samples(path: Path) -> Iterator[object]:
    """Decode the entire array one sample at a time, without retaining it.

    Each unfinished sample doubles the buffered text before another decode,
    bounding repeated work by its size. Consumed prefixes are discarded when
    reading more; ordinary complete samples use the standard C decoder once.
    """
    decoder = json.JSONDecoder(
        object_pairs_hook=reject_duplicate_json_keys,
        parse_constant=reject_nonstandard_json_constant,
    )
    whitespace = re.compile(r"[ \t\r\n]*")
    buffer = ""
    position = 0
    eof = False
    chunk_size = 64 * 1024

    with path.open("r", encoding="utf-8") as handle:
        def read_more(grow: bool = False) -> None:
            nonlocal buffer, position, eof
            remaining = buffer[position:]
            amount = max(chunk_size, len(remaining)) if grow else chunk_size
            text = handle.read(amount)
            buffer = remaining + text
            position = 0
            eof = not text

        def skip_whitespace() -> None:
            nonlocal position
            while True:
                position = whitespace.match(buffer, position).end()
                if position < len(buffer) or eof:
                    return
                read_more()

        def incomplete(exc: json.JSONDecodeError) -> bool:
            if exc.pos >= len(buffer) - 1 or exc.msg.startswith("Unterminated string"):
                return True
            tail = buffer[exc.pos:]
            if tail in {"t", "tr", "tru", "f", "fa", "fal", "fals", "n", "nu", "nul"}:
                return True
            if re.fullmatch(r"[eE][+-]?", tail):
                return True
            if exc.msg == "Invalid \\uXXXX escape" and re.fullmatch(r"u[0-9a-fA-F]{0,4}", tail):
                return True
            return False

        skip_whitespace()
        if position == len(buffer) or buffer[position] != "[":
            raise SystemExit(f"{path} does not contain a JSON array.")
        position += 1
        skip_whitespace()
        if position < len(buffer) and buffer[position] == "]":
            position += 1
        else:
            while True:
                skip_whitespace()
                while True:
                    try:
                        item, end = decoder.raw_decode(buffer, position)
                        break
                    except DuplicateJsonKeyError as exc:
                        raise SystemExit(f"{path} contains duplicate JSON key {exc.key!r}.") from exc
                    except InvalidJsonConstantError as exc:
                        raise SystemExit(f"{path} contains non-standard JSON constant {exc.value!r}.") from exc
                    except json.JSONDecodeError as exc:
                        if eof or not incomplete(exc):
                            raise
                        read_more(grow=True)
                position = end
                yield item
                skip_whitespace()
                if position == len(buffer):
                    raise json.JSONDecodeError("Unterminated JSON array", buffer, position)
                separator = buffer[position]
                position += 1
                if separator == "]":
                    break
                if separator != ",":
                    raise json.JSONDecodeError("Expected ',' or ']'", buffer, position - 1)
                skip_whitespace()
                if position < len(buffer) and buffer[position] == "]":
                    raise json.JSONDecodeError("Expected a JSON sample", buffer, position)
        skip_whitespace()
        if position != len(buffer):
            raise json.JSONDecodeError("Extra data after JSON array", buffer, position)


def load_json_export(path: Path) -> object:
    """Compatibility helper; plotting loaders consume iter_json_samples."""
    return list(iter_json_samples(path))


def streaming_sample_bounds(expr: Optional[str]) -> Tuple[int, Optional[int]]:
    """Parse syntax now and check the input's actual length after scanning."""
    if not expr:
        return 1, None
    sep = ":" if ":" in expr else "-"
    if sep not in expr:
        raise SystemExit("Use --sample-range START:END, for example 10:100.")
    start_text, end_text = expr.split(sep, 1)
    try:
        start = int(start_text) if start_text else 1
        end = int(end_text) if end_text else None
    except ValueError as exc:
        raise SystemExit("Invalid --sample-range. Expected one-based START:END.") from exc
    if start < 1 or (end is not None and end < start):
        raise SystemExit("Invalid --sample-range. Expected one-based START:END.")
    return start, end


def sample_is_selected(index: int, bounds: Tuple[int, Optional[int]]) -> bool:
    start, end = bounds
    return index >= start and (end is None or index <= end)


class FieldProjection:
    """Retain the candidates used by resolve_field_name, without all values."""

    def __init__(self, fields: Optional[Iterable[str]]):
        self.targets = None if fields is None else {
            normalize_field_name(field) for field in fields
        }
        if self.targets is not None:
            self.targets.update(FIELD_ALIASES.get(field, field)
                                for field in tuple(self.targets))

    def includes(self, field: str) -> bool:
        if self.targets is None:
            return True
        normalized = normalize_field_name(field)
        return (normalized in self.targets or
                normalized.rsplit(".", 1)[-1] in self.targets)

    def flatten(self, prefix: str, value: object,
                out: Dict[str, object]) -> None:
        if self.targets is None:
            flatten_dict(prefix, value, out)
        elif isinstance(value, dict):
            for key, nested in value.items():
                name = f"{prefix}.{key}" if prefix else key
                self.flatten(name, nested, out)
        elif self.includes(prefix):
            out[prefix] = value

def parse_timestamp_timezone(timestamp_iso: object):
    if not isinstance(timestamp_iso, str) or not timestamp_iso.strip():
        return timezone.utc

    value = timestamp_iso.strip()
    if value.endswith(("Z", "z")):
        value = f"{value[:-1]}+00:00"
    # Normalize compact ISO offsets accepted by the loaders on newer Python.
    value = re.sub(r"([+-]\d{2})(\d{2})$", r"\1:\2", value)
    # Python 3.10 only accepts 3 or 6 fractional digits. The exporter emits
    # nanoseconds; normalize just this parsing copy to recover its UTC offset.
    value = re.sub(
        r"\.(\d+)(?=[+-]\d{2}:\d{2}$)",
        lambda match: "." + match.group(1)[:6].ljust(6, "0"),
        value,
    )
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError:
        return timezone.utc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return timezone.utc
    return parsed.tzinfo


def sample_x_value(timestamp: object, sample_index: int,
                   timestamp_ns: object = None,
                   timestamp_iso: object = None) -> object:
    sample_timezone = parse_timestamp_timezone(timestamp_iso)
    try:
        if timestamp_ns not in (None, "") and is_finite_number(timestamp_ns):
            return datetime.fromtimestamp(
                to_float(timestamp_ns) / 1_000_000_000.0,
                tz=sample_timezone,
            )
        if is_finite_number(timestamp):
            return datetime.fromtimestamp(to_float(timestamp), tz=sample_timezone)
    except (OverflowError, OSError, ValueError):
        pass
    return sample_index


def finalize_x_axis(x_values: List[object],
                    sample_values: List[int]) -> Tuple[List[object], str]:
    if x_values and all(isinstance(value, datetime) for value in x_values):
        if all(left < right for left, right in zip(x_values, x_values[1:])):
            offsets = {value.utcoffset() for value in x_values}
            if len(offsets) == 1:
                offset = next(iter(offsets)) or timedelta(0)
                total_minutes = int(offset.total_seconds() // 60)
                if total_minutes == 0:
                    zone_label = "UTC"
                else:
                    sign = "+" if total_minutes >= 0 else "-"
                    absolute_minutes = abs(total_minutes)
                    zone_label = (
                        f"UTC{sign}{absolute_minutes // 60:02d}:"
                        f"{absolute_minutes % 60:02d}"
                    )
                return x_values, f"time ({zone_label})"

            utc_values = [value.astimezone(timezone.utc) for value in x_values]
            return utc_values, "time (UTC)"

    return list(sample_values), "sample"


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
                    if is_finite_number(value):
                        seen.setdefault(key, None)
        else:
            for key, value in item.items():
                if key.startswith("__"):
                    continue
                if is_finite_number(value):
                    seen.setdefault(key, None)
    return sorted(seen.keys())


def parse_cpu_id(value: object, source: Path) -> int:
    if not is_finite_number(value):
        raise SystemExit(f"{source} contains an invalid CPU ID: {value!r}.")

    numeric = to_float(value)
    if not numeric.is_integer() or numeric < 0:
        raise SystemExit(f"{source} contains an invalid CPU ID: {value!r}.")
    return int(numeric)


def validate_interval_number(value: object, expected: int, source: Path) -> None:
    if not is_finite_number(value):
        raise SystemExit(f"{source} contains an invalid interval: {value!r}.")

    numeric = to_float(value)
    if not numeric.is_integer() or int(numeric) != expected:
        raise SystemExit(
            f"{source} contains interval {value!r}; expected {expected}."
        )


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

def count_csv_summary_samples(path: Path) -> int:
    count = 0
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        fieldnames = validate_csv_structure(reader, path, "summary")
        require_csv_columns(
            fieldnames,
            {"schema_version", "interval", "Scope"},
            path,
            "summary",
            "armstat -S -f csv -o summary.csv",
        )
        for item in reader:
            validate_csv_row_shape(item, path, reader.line_num)
            if item.get("Scope") == "SUM":
                validate_schema_version(item.get("schema_version"), path)
                validate_interval_number(item.get("interval"), count + 1, path)
                count += 1
    return count


def count_csv_cpu_samples(path: Path) -> int:
    count = 0
    current_key = None
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        fieldnames = validate_csv_structure(reader, path, "per-CPU")
        require_csv_columns(
            fieldnames,
            {"schema_version", "interval", "CPU"},
            path,
            "per-CPU",
            "armstat -a -f csv -o cpus.csv",
        )
        is_mixed_scope = reader.fieldnames and "Scope" in reader.fieldnames
        for item in reader:
            validate_csv_row_shape(item, path, reader.line_num)
            if is_mixed_scope and item.get("Scope") != "CPU":
                continue
            if item.get("CPU") in (None, ""):
                raise SystemExit(
                    f"{path} contains a per-CPU row without a CPU ID."
                )
            validate_schema_version(item.get("schema_version"), path)
            key = (
                item.get("interval"), item.get("timestamp"),
                item.get("timestamp_ns"), item.get("timestamp_iso"),
            )
            if key != current_key:
                count += 1
                validate_interval_number(item.get("interval"), count, path)
                current_key = key
    return count


def validate_csv_structure(reader: csv.DictReader,
                           path: Path,
                           export_name: str) -> List[str]:
    fieldnames = reader.fieldnames
    if not fieldnames:
        raise SystemExit(f"{path} does not contain a CSV header.")

    seen: Set[str] = set()
    duplicates: Set[str] = set()
    for field in fieldnames:
        if field in seen:
            duplicates.add(field)
        seen.add(field)
    if duplicates:
        names = ", ".join(sorted(duplicates))
        raise SystemExit(
            f"{path} contains duplicate {export_name} CSV columns: {names}."
        )

    return fieldnames


def require_csv_columns(fieldnames: Iterable[str], required: Set[str],
                        path: Path, export_name: str, example: str) -> None:
    missing_columns = required.difference(fieldnames)
    if missing_columns:
        raise SystemExit(
            f"{path} is missing required {export_name} CSV columns: "
            f"{', '.join(sorted(missing_columns))}. "
            f"Use {export_name} CSV export, for example: {example}"
        )


def validate_csv_row_shape(item: Dict[Optional[str], object],
                           path: Path,
                           line_number: int) -> None:
    if None in item:
        raise SystemExit(
            f"{path} line {line_number} contains more cells than its CSV header."
        )
    missing = [key for key, value in item.items() if value is None]
    if missing:
        raise SystemExit(
            f"{path} line {line_number} contains fewer cells than its CSV header "
            f"(missing: {', '.join(str(key) for key in missing)})."
        )


# ---------------------------------------------------------------------------
# Summary loaders
# ---------------------------------------------------------------------------

def load_json_summary(path: Path, sample_range: Optional[str] = None,
                      fields: Optional[Iterable[str]] = None,
                      *, ndjson: bool = False, recover_tail: bool = False) -> SeriesData:
    bounds = streaming_sample_bounds(sample_range)
    projection = FieldProjection(fields)
    rows: List[Dict[str, object]] = []
    x_values: List[object] = []
    sample_values: List[int] = []
    total_samples = 0

    source = iter_ndjson_samples(path, recover_tail) if ndjson else iter_json_samples(path)
    for sample_index, item in enumerate(source, start=1):
        total_samples = sample_index
        if not isinstance(item, dict):
            raise SystemExit(
                f"{path} sample {sample_index} is not a JSON object."
            )
        validate_schema_version(item.get("schema_version"), path)
        summary = item.get("summary")
        if not isinstance(summary, dict):
            raise SystemExit(
                f"{path} sample {sample_index} does not contain a summary object."
            )
        validate_interval_number(item.get("interval"), sample_index, path)
        if not sample_is_selected(sample_index, bounds):
            continue

        row: Dict[str, object] = {}
        projection.flatten("", summary, row)
        row["__interval_index"] = sample_index
        row["__timestamp"] = item.get("timestamp")
        rows.append(row)

        x_values.append(sample_x_value(
            item.get("timestamp"), sample_index, item.get("timestamp_ns"),
            item.get("timestamp_iso")
        ))
        sample_values.append(sample_index)

    parse_sample_range(sample_range, total_samples)
    if not rows:
        raise SystemExit(
            f"{path} does not contain summary records. "
            "Use summary JSON export, for example: armstat -S -f json -o summary.json"
        )

    x_values, x_label = finalize_x_axis(x_values, sample_values)
    return SeriesData(
        x_values=x_values,
        x_label=x_label,
        rows=rows,
        numeric_fields=collect_numeric_fields(rows),
    )


CSV_METADATA_COLUMNS = {
    "Scope", "CPU", "Package", "schema_version", "interval", "duration_us",
    "timestamp", "timestamp_ns", "timestamp_iso",
}


def csv_metric_columns(fieldnames: Iterable[str], mixed: bool, scope: str,
                       projection: FieldProjection) -> List[Tuple[str, str]]:
    """Discard other scopes and resolve column names once, before row loading."""
    columns: List[Tuple[str, str]] = []
    excluded = ("cpu.", "package.") if scope == "summary" else ("summary.", "package.")
    for key in fieldnames:
        if key in CSV_METADATA_COLUMNS:
            continue
        if mixed and normalize_field_name(key).startswith(excluded):
            continue
        canonical = canonicalize_csv_key(key)
        if projection.includes(canonical):
            columns.append((key, canonical))
    return columns


def load_csv_summary(path: Path, sample_range: Optional[str] = None,
                     fields: Optional[Iterable[str]] = None) -> SeriesData:
    bounds = streaming_sample_bounds(sample_range)
    projection = FieldProjection(fields)
    rows: List[Dict[str, object]] = []
    x_values: List[object] = []
    sample_values: List[int] = []

    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        fieldnames = validate_csv_structure(reader, path, "summary")
        require_csv_columns(
            fieldnames,
            {"schema_version", "interval", "Scope"},
            path,
            "summary",
            "armstat -S -f csv -o summary.csv",
        )
        is_mixed_scope = {"CPU", "Package"}.issubset(fieldnames)
        metric_columns = csv_metric_columns(fieldnames, is_mixed_scope, "summary", projection)

        sample_index = 0
        for item in reader:
            validate_csv_row_shape(item, path, reader.line_num)
            if item.get("Scope") != "SUM":
                continue
            sample_index += 1
            validate_interval_number(item.get("interval"), sample_index, path)
            validate_schema_version(item.get("schema_version"), path)
            if not sample_is_selected(sample_index, bounds):
                continue

            row = {canonical: item[key] for key, canonical in metric_columns}
            row["__interval_index"] = sample_index
            row["__timestamp"] = item.get("timestamp")
            rows.append(row)

            x_values.append(sample_x_value(
                item.get("timestamp"), sample_index, item.get("timestamp_ns"),
                item.get("timestamp_iso")
            ))
            sample_values.append(sample_index)

    parse_sample_range(sample_range, sample_index)
    if not rows:
        raise SystemExit(f"{path} does not contain summary rows.")

    x_values, x_label = finalize_x_axis(x_values, sample_values)
    return SeriesData(
        x_values=x_values,
        x_label=x_label,
        rows=rows,
        numeric_fields=collect_numeric_fields(rows),
    )


def load_summary_series(path: Path, sample_range: Optional[str] = None,
                        fields: Optional[Iterable[str]] = None,
                        *, recover_tail: bool = False) -> SeriesData:
    try:
        export_format = detect_export_format(path)
        if recover_tail and export_format != "ndjson":
            raise SystemExit("--recover-tail is only supported for NDJSON input.")
        if export_format in {"json", "ndjson"}:
            return load_json_summary(path, sample_range, fields,
                                     ndjson=export_format == "ndjson", recover_tail=recover_tail)
        if export_format == "csv":
            return load_csv_summary(path, sample_range, fields)
        raise SystemExit(
            f"Unsupported input format for {path}. "
            "Use summary JSON, NDJSON, or CSV exported by armstat."
        )
    except (OSError, UnicodeError, json.JSONDecodeError, csv.Error) as exc:
        raise SystemExit(f"Could not read {path}: {exc}") from exc


def slice_summary_series(series: SeriesData,
                         sample_range: Optional[str]) -> SeriesData:
    parsed = parse_sample_range(sample_range, len(series.rows))
    if parsed is None:
        return series
    start, end = parsed
    start_idx = start - 1
    end_idx = end
    sliced_rows = series.rows[start_idx:end_idx]
    return SeriesData(
        x_values=series.x_values[start_idx:end_idx],
        x_label=series.x_label,
        rows=sliced_rows,
        numeric_fields=collect_numeric_fields(sliced_rows),
    )


# ---------------------------------------------------------------------------
# CPU loaders
# ---------------------------------------------------------------------------

def load_json_cpu_series(path: Path, sample_range: Optional[str] = None,
                         cpu_filter: Optional[Iterable[int]] = None,
                         fields: Optional[Iterable[str]] = None,
                         *, ndjson: bool = False, recover_tail: bool = False) -> CpuSeriesData:
    bounds = streaming_sample_bounds(sample_range)
    selected_cpus = None if cpu_filter is None else set(cpu_filter)
    projection = FieldProjection(fields)
    samples: List[Dict[int, Dict[str, object]]] = []
    x_values: List[object] = []
    sample_values: List[int] = []
    cpu_ids: Set[int] = set()
    total_samples = 0

    source = iter_ndjson_samples(path, recover_tail) if ndjson else iter_json_samples(path)
    for sample_index, item in enumerate(source, start=1):
        total_samples = sample_index
        if not isinstance(item, dict):
            raise SystemExit(
                f"{path} sample {sample_index} is not a JSON object."
            )
        validate_schema_version(item.get("schema_version"), path)
        cpus = item.get("cpus")
        if not isinstance(cpus, list):
            raise SystemExit(
                f"{path} sample {sample_index} does not contain a cpus array."
            )
        validate_interval_number(item.get("interval"), sample_index, path)

        sample: Dict[int, Dict[str, object]] = {}
        seen_cpu_ids: Set[int] = set()
        keep_sample = sample_is_selected(sample_index, bounds)
        for entry_index, cpu_entry in enumerate(cpus, start=1):
            if not isinstance(cpu_entry, dict):
                raise SystemExit(
                    f"{path} sample {sample_index} CPU entry {entry_index} "
                    "is not a JSON object."
                )
            cpu_id = parse_cpu_id(cpu_entry.get("cpu"), path)
            if cpu_id in seen_cpu_ids:
                raise SystemExit(
                    f"{path} sample {sample_index} contains duplicate CPU ID "
                    f"{cpu_id}."
                )
            seen_cpu_ids.add(cpu_id)
            if not keep_sample or (selected_cpus is not None and cpu_id not in selected_cpus):
                continue
            row: Dict[str, object] = {}
            for key, value in cpu_entry.items():
                if key == "cpu":
                    continue
                projection.flatten(key, value, row)

            sample[cpu_id] = row
            cpu_ids.add(cpu_id)

        if not cpus:
            raise SystemExit(
                f"{path} sample {sample_index} does not contain any CPU rows."
            )
        if not keep_sample:
            continue

        samples.append(sample)
        x_values.append(sample_x_value(
            item.get("timestamp"), sample_index, item.get("timestamp_ns"),
            item.get("timestamp_iso")
        ))
        sample_values.append(sample_index)

    parse_sample_range(sample_range, total_samples)
    if not samples:
        raise SystemExit(
            f"{path} does not contain per-CPU JSON records. "
            "Use CPU JSON export, for example: armstat -a -f json -o cpus.json"
        )

    x_values, x_label = finalize_x_axis(x_values, sample_values)
    return CpuSeriesData(
        x_values=x_values,
        x_label=x_label,
        samples=samples,
        cpu_ids=sorted(cpu_ids),
        numeric_fields=collect_numeric_fields(samples),
    )


def load_csv_cpu_series(path: Path, sample_range: Optional[str] = None,
                        cpu_filter: Optional[Iterable[int]] = None,
                        fields: Optional[Iterable[str]] = None) -> CpuSeriesData:
    bounds = streaming_sample_bounds(sample_range)
    selected_cpus = None if cpu_filter is None else set(cpu_filter)
    projection = FieldProjection(fields)
    samples: List[Dict[int, Dict[str, object]]] = []
    x_values: List[object] = []
    sample_values: List[int] = []
    cpu_ids: Set[int] = set()

    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        fieldnames = validate_csv_structure(reader, path, "per-CPU")

        is_mixed_scope = "Scope" in fieldnames
        require_csv_columns(
            fieldnames,
            {"schema_version", "interval", "CPU"},
            path,
            "per-CPU",
            "armstat -a -f csv -o cpus.csv",
        )
        metric_columns = csv_metric_columns(fieldnames, is_mixed_scope, "cpu", projection)

        current_key: Optional[Tuple[object, object, object, object]] = None
        current_sample: Dict[int, Dict[str, object]] = {}
        seen_cpu_ids: Set[int] = set()
        sample_index = 0

        def flush_sample() -> None:
            nonlocal current_sample, seen_cpu_ids, sample_index
            if current_key is None:
                return
            sample_index += 1
            if sample_is_selected(sample_index, bounds):
                samples.append(current_sample)
                x_values.append(sample_x_value(
                    current_key[1], sample_index, current_key[2], current_key[3]
                ))
                sample_values.append(sample_index)
            current_sample = {}
            seen_cpu_ids = set()

        for item in reader:
            validate_csv_row_shape(item, path, reader.line_num)
            if is_mixed_scope and item.get("Scope") != "CPU":
                continue

            cpu_value = item.get("CPU")
            if cpu_value in (None, ""):
                raise SystemExit(f"{path} contains a per-CPU row without a CPU ID.")
            validate_schema_version(item.get("schema_version"), path)

            key = (
                item.get("interval"), item.get("timestamp"),
                item.get("timestamp_ns"), item.get("timestamp_iso"),
            )
            if current_key is None:
                validate_interval_number(item.get("interval"), 1, path)
                current_key = key
            elif key != current_key:
                flush_sample()
                validate_interval_number(
                    item.get("interval"), sample_index + 1, path
                )
                current_key = key

            cpu_id = parse_cpu_id(cpu_value, path)
            if cpu_id in seen_cpu_ids:
                raise SystemExit(
                    f"{path} sample {sample_index + 1} contains duplicate "
                    f"CPU ID {cpu_id}."
                )
            seen_cpu_ids.add(cpu_id)
            if (not sample_is_selected(sample_index + 1, bounds) or
                    (selected_cpus is not None and cpu_id not in selected_cpus)):
                continue
            row = {canonical: item[key] for key, canonical in metric_columns}
            current_sample[cpu_id] = row

        flush_sample()

    parse_sample_range(sample_range, sample_index)
    if not samples:
        raise SystemExit(f"{path} does not contain per-CPU rows.")

    for sample in samples:
        cpu_ids.update(sample.keys())

    x_values, x_label = finalize_x_axis(x_values, sample_values)
    return CpuSeriesData(
        x_values=x_values,
        x_label=x_label,
        samples=samples,
        cpu_ids=sorted(cpu_ids),
        numeric_fields=collect_numeric_fields(samples),
    )


def load_cpu_series(path: Path, sample_range: Optional[str] = None,
                    cpu_filter: Optional[Iterable[int]] = None,
                    fields: Optional[Iterable[str]] = None,
                    *, recover_tail: bool = False) -> CpuSeriesData:
    try:
        export_format = detect_export_format(path)
        if recover_tail and export_format != "ndjson":
            raise SystemExit("--recover-tail is only supported for NDJSON input.")
        if export_format in {"json", "ndjson"}:
            return load_json_cpu_series(path, sample_range, cpu_filter, fields,
                                        ndjson=export_format == "ndjson", recover_tail=recover_tail)
        if export_format == "csv":
            return load_csv_cpu_series(path, sample_range, cpu_filter, fields)
        raise SystemExit(
            f"Unsupported input format for {path}. "
            "Use CPU JSON, NDJSON, or CSV exported by armstat."
        )
    except (OSError, UnicodeError, json.JSONDecodeError, csv.Error) as exc:
        raise SystemExit(f"Could not read {path}: {exc}") from exc


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
