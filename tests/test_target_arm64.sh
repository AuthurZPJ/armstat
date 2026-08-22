#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

armstat_bin=${ARMSTAT_BIN:-./armstat}
sample_interval=${ARMSTAT_TARGET_INTERVAL:-0.1}
sample_count=${ARMSTAT_TARGET_SAMPLES:-5}
soak_iterations=${ARMSTAT_SOAK_ITERATIONS:-0}
soak_interval=${ARMSTAT_SOAK_INTERVAL:-1}
max_rss_kib=${ARMSTAT_MAX_RSS_KIB:-262144}
max_open_fds=${ARMSTAT_MAX_OPEN_FDS:-256}
max_diagnostic_lines=${ARMSTAT_MAX_DIAGNOSTIC_LINES:-16}
require_pmu=${ARMSTAT_REQUIRE_PMU:-0}
require_cpuidle=${ARMSTAT_REQUIRE_CPUIDLE:-0}
require_power=${ARMSTAT_REQUIRE_POWER:-0}
require_temp=${ARMSTAT_REQUIRE_TEMP:-0}
require_membw=${ARMSTAT_REQUIRE_MEMBW:-0}
require_uncore=${ARMSTAT_REQUIRE_UNCORE:-0}
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/armstat-target.XXXXXX")
child_pid=

cleanup()
{
	if [ -n "$child_pid" ] && kill -0 "$child_pid" 2>/dev/null; then
		kill -TERM "$child_pid" 2>/dev/null || true
		wait "$child_pid" 2>/dev/null || true
	fi
	rm -rf "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

require_positive_integer()
{
	name=$1
	value=$2
	case $value in
	''|*[!0-9]*|0)
		echo "$name must be a positive integer" >&2
		exit 1
		;;
	esac
}

require_boolean()
{
	name=$1
	value=$2
	case $value in
	0|1)
		;;
	*)
		echo "$name must be 0 or 1" >&2
		exit 1
		;;
	esac
}

probe_value()
{
	awk -v key="$1:" '$1 == key { print $2; exit }' "$tmp_dir/probe.txt"
}

require_probe_yes()
{
	name=$1
	value=$2
	if [ "$value" != yes ]; then
		echo "target-test requires $name, but --probe reported ${value:-missing}" >&2
		exit 1
	fi
}

case $soak_iterations in
''|*[!0-9]*)
	echo "ARMSTAT_SOAK_ITERATIONS must be a non-negative integer" >&2
	exit 1
	;;
esac
require_positive_integer ARMSTAT_TARGET_SAMPLES "$sample_count"
require_positive_integer ARMSTAT_MAX_RSS_KIB "$max_rss_kib"
require_positive_integer ARMSTAT_MAX_OPEN_FDS "$max_open_fds"
require_positive_integer ARMSTAT_MAX_DIAGNOSTIC_LINES "$max_diagnostic_lines"
require_boolean ARMSTAT_REQUIRE_PMU "$require_pmu"
require_boolean ARMSTAT_REQUIRE_CPUIDLE "$require_cpuidle"
require_boolean ARMSTAT_REQUIRE_POWER "$require_power"
require_boolean ARMSTAT_REQUIRE_TEMP "$require_temp"
require_boolean ARMSTAT_REQUIRE_MEMBW "$require_membw"
require_boolean ARMSTAT_REQUIRE_UNCORE "$require_uncore"

wait_for_sampling_ready()
{
	ready_pid=$1
	attempt=0
	while kill -0 "$ready_pid" 2>/dev/null && [ "$attempt" -lt 300 ]; do
		for fd_path in "/proc/$ready_pid/fd/"*; do
			fd_target=$(readlink "$fd_path" 2>/dev/null || true)
			if [ "$fd_target" = "/proc/stat" ]; then
				return 0
			fi
		done
		attempt=$((attempt + 1))
		sleep 0.1
	done

	echo "target-test timed out waiting for armstat sampling readiness" >&2
	return 1
}

case $(uname -s) in
Linux)
	;;
*)
	echo "target-test requires Linux" >&2
	exit 1
	;;
esac

case $(uname -m) in
aarch64|arm64)
	;;
*)
	echo "target-test requires an ARM64 host" >&2
	exit 1
	;;
esac

test -x "$armstat_bin"

for early_option in --help --version --list; do
	if "$armstat_bin" "$early_option" >/dev/full 2>"$tmp_dir/full.stderr"; then
		echo "target-test $early_option ignored an output write failure" >&2
		exit 1
	fi
	grep -F 'failed to finalize output' "$tmp_dir/full.stderr" >/dev/null
done

python3 - "$armstat_bin" "$sample_interval" <<'PY'
import subprocess
import sys

binary, interval = sys.argv[1:]
process = subprocess.Popen(
    [binary, "-S", "-f", "json", "-i", interval, "-n", "1"],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
assert process.stdout is not None
assert process.stderr is not None
process.stdout.close()
stderr = process.stderr.read().decode("utf-8", errors="replace")
returncode = process.wait()
assert returncode == 1, (returncode, stderr)
assert "failed to finalize output" in stderr, stderr
PY

"$armstat_bin" --probe >"$tmp_dir/probe.txt"
probe_schema_version=$(probe_value probe_schema_version)
online_cpus=$(probe_value online_cpus)
tracked_cpus=$(probe_value tracked_cpus)
if [ "$probe_schema_version" != 1 ]; then
	echo "target-test requires probe_schema_version 1" >&2
	exit 1
fi
case $online_cpus:$tracked_cpus in
*[!0-9:]*|:*)
	echo "target-test could not parse probe CPU counts" >&2
	exit 1
	;;
esac
test "$online_cpus" -gt 0
test "$tracked_cpus" -gt 0
test "$tracked_cpus" -le "$online_cpus"

printf '%s\n' 'previous-valid-probe' >"$tmp_dir/temp-policy.out"
if ARMSTAT_TEMP_POLICY=invalid "$armstat_bin" --probe \
	-o "$tmp_dir/temp-policy.out" 2>"$tmp_dir/temp-policy.stderr"; then
	echo "target-test invalid ARMSTAT_TEMP_POLICY unexpectedly succeeded" >&2
	exit 1
fi
grep -F "unknown ARMSTAT_TEMP_POLICY 'invalid'" \
	"$tmp_dir/temp-policy.stderr" >/dev/null
grep -Fx 'previous-valid-probe' "$tmp_dir/temp-policy.out" >/dev/null

idle_states=$(probe_value idle_states)
numa_temp_sensors=$(probe_value numa_temp_sensors)
numa_temp_mask=$(probe_value numa_temp_mask)
package_power_mw=$(probe_value package_power_mw)
package_power_candidates=$(probe_value package_power_candidates)
pmu_cycles=$(probe_value pmu_cycles)
mem_bw_supported=$(probe_value mem_bw_supported)
mem_bw_candidates=$(probe_value mem_bw_candidates)
uncore_freq_supported=$(probe_value uncore_freq_supported)

case $idle_states:$numa_temp_sensors in
*[!0-9:]*|:*|*:)
	echo "target-test could not parse probe idle/temperature counts" >&2
	exit 1
	;;
esac
case $numa_temp_mask in
0x[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F])
	;;
*)
	echo "target-test could not parse probe temperature mask" >&2
	exit 1
	;;
esac
python3 - "$numa_temp_sensors" "$numa_temp_mask" <<'PY'
import sys

sensor_count = int(sys.argv[1])
sensor_mask = int(sys.argv[2], 16)
assert bin(sensor_mask).count("1") == sensor_count, (sensor_count, sensor_mask)
PY
case $package_power_mw in
unavailable)
	;;
''|*[!0-9]*)
	echo "target-test could not parse probe package power" >&2
	exit 1
	;;
	esac
case $package_power_candidates:$mem_bw_candidates in
*[!0-9:]*|:*|*:)
	echo "target-test could not parse sensor candidate counts" >&2
	exit 1
	;;
esac
case $pmu_cycles:$mem_bw_supported:$uncore_freq_supported in
yes:yes:yes|yes:yes:no|yes:no:yes|yes:no:no|no:yes:yes|no:yes:no|no:no:yes|no:no:no)
	;;
*)
	echo "target-test could not parse probe capability flags" >&2
	exit 1
	;;
	esac
if [ "$package_power_candidates" -ne 1 ]; then
	test "$package_power_mw" = unavailable
	test -z "$(probe_value package_power_source)"
fi
if [ "$package_power_candidates" -gt 1 ]; then
	grep -F 'package_power_note: ambiguous; expected exactly one' \
		"$tmp_dir/probe.txt" >/dev/null
fi
if [ "$mem_bw_candidates" -eq 1 ]; then
	test "$mem_bw_supported" = yes
else
	test "$mem_bw_supported" = no
	test -z "$(probe_value mem_bw_source)"
fi
if [ "$mem_bw_candidates" -gt 1 ]; then
	grep -F 'mem_bw_note: ambiguous; expected exactly one' \
		"$tmp_dir/probe.txt" >/dev/null
fi
if [ "$pmu_cycles" = no ]; then
	grep -F 'pmu_note: unavailable; check perf permissions, kernel PMU support' \
		"$tmp_dir/probe.txt" >/dev/null
fi

if [ "$require_pmu" -eq 1 ]; then
	require_probe_yes PMU "$pmu_cycles"
fi
if [ "$require_cpuidle" -eq 1 ] && [ "$idle_states" -eq 0 ]; then
	echo "target-test requires cpuidle states, but --probe reported none" >&2
	exit 1
fi
if [ "$require_power" -eq 1 ] && [ "$package_power_mw" = unavailable ]; then
	echo "target-test requires package power, but --probe reported unavailable" >&2
	exit 1
fi
if [ "$require_power" -eq 1 ]; then
	package_power_source=$(probe_value package_power_source)
	case $package_power_source in
	/*)
		;;
	*)
		echo "target-test requires an identified package power source" >&2
		exit 1
		;;
	esac
fi
if [ "$require_temp" -eq 1 ] && [ "$numa_temp_sensors" -eq 0 ]; then
	echo "target-test requires temperature sensors, but --probe reported none" >&2
	exit 1
fi
if [ "$require_membw" -eq 1 ]; then
	require_probe_yes "memory bandwidth" "$mem_bw_supported"
	mem_bw_source=$(probe_value mem_bw_source)
	case $mem_bw_source in
	/*)
		;;
	*)
		echo "target-test requires an identified memory bandwidth source" >&2
		exit 1
		;;
	esac
fi
if [ "$require_uncore" -eq 1 ]; then
	require_probe_yes "uncore frequency" "$uncore_freq_supported"
	uncore_freq_mhz=$(probe_value uncore_freq_mhz)
	if ! awk -v value="$uncore_freq_mhz" \
		'BEGIN { exit value !~ /^[0-9]+([.][0-9]+)?$/ }'; then
		echo "target-test requires a readable uncore frequency" >&2
		exit 1
	fi
fi

if "$armstat_bin" --probe -o /dev/full 2>"$tmp_dir/probe-full.stderr"; then
	echo "target-test --probe ignored an output write failure" >&2
	exit 1
fi
grep -F 'failed to finalize output' "$tmp_dir/probe-full.stderr" >/dev/null

"$armstat_bin" -f json -i "$sample_interval" -n 1 \
	>"$tmp_dir/default.json"
python3 - "$tmp_dir/default.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    samples = json.load(stream)

assert len(samples) == 1
sample = samples[0]
assert sample.get("schema_version") == 8
assert isinstance(sample.get("summary"), dict)
assert isinstance(sample.get("packages"), list) and sample["packages"]
assert "cpus" not in sample
PY

printf '%s\n' 'previous-valid-export' >"$tmp_dir/existing.out"
if "$armstat_bin" -S -s cpu -o "$tmp_dir/existing.out" -i "$sample_interval" \
	-n 1 >"$tmp_dir/invalid.stdout" 2>"$tmp_dir/invalid.stderr"; then
	echo "target-test mode-incompatible selection unexpectedly succeeded" >&2
	exit 1
fi
grep -F 'cannot produce data' "$tmp_dir/invalid.stderr" >/dev/null
grep -Fx 'previous-valid-export' "$tmp_dir/existing.out" >/dev/null

"$armstat_bin" -S -a -f json -i "$sample_interval" -n "$sample_count" \
	>"$tmp_dir/samples.json"
python3 - "$tmp_dir/samples.json" "$sample_count" <<'PY'
import json
from datetime import datetime
import re
import sys

def reject_constant(value):
    raise ValueError(f"non-standard JSON constant: {value}")

def parse_rfc3339_ns(value):
    assert re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{9}[+-]\d{2}:\d{2}",
        value,
    ), value
    # Older Python accepts at most microseconds in fromisoformat().
    return datetime.fromisoformat(value[:26] + value[29:])

path, expected_text = sys.argv[1:]
with open(path, encoding="utf-8") as stream:
    samples = json.load(stream, parse_constant=reject_constant)
expected = int(expected_text)
assert len(samples) == expected, (len(samples), expected)
assert all(sample.get("schema_version") == 8 for sample in samples)
assert [sample.get("interval") for sample in samples] == list(range(1, expected + 1))
assert all(isinstance(sample.get("duration_us"), int) and
           sample["duration_us"] > 0 for sample in samples)
assert all(isinstance(sample.get("timestamp"), int) for sample in samples)
assert all(isinstance(sample.get("timestamp_ns"), int) for sample in samples)
assert all(sample["timestamp_ns"] // 1_000_000_000 == sample["timestamp"]
           for sample in samples)
assert all(left["timestamp_ns"] < right["timestamp_ns"]
           for left, right in zip(samples, samples[1:]))
assert all(isinstance(sample.get("timestamp_iso"), str) for sample in samples)
assert all("." in sample["timestamp_iso"] for sample in samples)
assert all(parse_rfc3339_ns(sample["timestamp_iso"])
           for sample in samples)
PY

printf '%s\n' 'previous-valid-export' >"$tmp_dir/option-output.json"
"$armstat_bin" -S -s ctx_switches -f json -i "$sample_interval" -n 1 \
	--output "$tmp_dir/option-output.json"
python3 - "$tmp_dir/option-output.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    samples = json.load(stream)

assert len(samples) == 1
assert samples[0].get("schema_version") == 8
assert isinstance(samples[0].get("summary", {}).get("ctx_switches"), int)
PY

"$armstat_bin" --probe --export "$tmp_dir/probe-option-output.txt"
grep -F 'probe_schema_version: 1' "$tmp_dir/probe-option-output.txt" >/dev/null

if "$armstat_bin" -S -s ctx_switches -f json -i "$sample_interval" -n 1 \
	--output "$tmp_dir" 2>"$tmp_dir/output-directory.stderr"; then
	echo "target-test writing to a directory unexpectedly succeeded" >&2
	exit 1
fi
grep -F 'cannot open output file' "$tmp_dir/output-directory.stderr" >/dev/null

"$armstat_bin" -a -f json -i "$sample_interval" -n 1 \
	>"$tmp_dir/mixed.json"
python3 - "$tmp_dir/mixed.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    samples = json.load(stream)

assert len(samples) == 1
sample = samples[0]
assert sample.get("schema_version") == 8
assert all("package_id" not in package
           for package in sample.get("packages", []))
for cpu in sample.get("cpus", []):
    assert cpu.get("governor") is None or isinstance(cpu["governor"], str)
    assert cpu.get("boost") is None or isinstance(cpu["boost"], bool)
PY

if [ "$require_pmu" -eq 1 ]; then
	"$armstat_bin" -S -I -f json -i "$sample_interval" -n "$sample_count" \
		>"$tmp_dir/required-pmu.json"
	python3 - "$tmp_dir/required-pmu.json" <<'PY'
import json
import math
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    samples = json.load(stream)

valid = []
for sample in samples:
    summary = sample.get("summary", {})
    pmu = summary.get("pmu")
    ipc = summary.get("ipc")
    if not isinstance(pmu, dict):
        continue
    cycles = pmu.get("cycles")
    instructions = pmu.get("instructions")
    numeric_ipc = (isinstance(ipc, (int, float)) and
                   not isinstance(ipc, bool) and math.isfinite(ipc))
    if (isinstance(cycles, int) and not isinstance(cycles, bool) and
            isinstance(instructions, int) and
            not isinstance(instructions, bool) and numeric_ipc):
        valid.append((cycles, instructions, ipc))

assert valid, "PMU/IPC remained unavailable during required sampling"
assert any(cycles > 0 and instructions > 0 and ipc >= 0
           for cycles, instructions, ipc in valid), valid
PY
fi

if [ "$require_cpuidle" -eq 1 ] || [ "$require_power" -eq 1 ] || \
	[ "$require_temp" -eq 1 ] || [ "$require_membw" -eq 1 ] || \
	[ "$require_uncore" -eq 1 ]; then
	"$armstat_bin" -S -s idle,power,temp,membw,uncore_freq -f json \
		-i "$sample_interval" -n "$sample_count" \
		>"$tmp_dir/required-telemetry.json"
	python3 - "$tmp_dir/required-telemetry.json" "$require_cpuidle" \
		"$require_power" "$require_temp" "$require_membw" \
		"$require_uncore" <<'PY'
import json
import math
import re
import sys

path = sys.argv[1]
require_idle, require_power, require_temp, require_membw, require_uncore = (
    value == "1" for value in sys.argv[2:]
)

with open(path, encoding="utf-8") as stream:
    summaries = [sample.get("summary", {}) for sample in json.load(stream)]

def finite_number(value):
    return (isinstance(value, (int, float)) and not isinstance(value, bool) and
            math.isfinite(value))

def require_any(name, values, predicate=lambda value: True):
    valid = [value for value in values
             if finite_number(value) and predicate(value)]
    assert valid, f"{name} remained unavailable during required sampling"

if require_idle:
    idle_values = [value for summary in summaries
                   for key, value in summary.items()
                   if re.fullmatch(r"lpi[0-7]", key)]
    require_any("cpuidle", idle_values, lambda value: 0 <= value <= 100)
if require_power:
    require_any("package power", [summary.get("power") for summary in summaries],
                lambda value: value >= 0)
if require_temp:
    temp_values = [value for summary in summaries
                   for key, value in summary.items()
                   if re.fullmatch(r"temp[0-3]", key)]
    require_any("temperature", temp_values)
if require_membw:
    require_any("memory bandwidth",
                [summary.get("mem_bw") for summary in summaries],
                lambda value: value >= 0)
if require_uncore:
    require_any("uncore frequency",
                [summary.get("uncore_freq") for summary in summaries],
                lambda value: value > 0)
PY
fi

"$armstat_bin" -S -a -f csv -i "$sample_interval" -n "$sample_count" \
	>"$tmp_dir/samples.csv"
python3 - "$tmp_dir/samples.csv" "$sample_count" <<'PY'
import csv
import sys

path, expected_text = sys.argv[1:]
with open(path, newline="", encoding="utf-8") as stream:
    rows = list(csv.reader(stream))
expected = int(expected_text)
assert len(rows) == expected + 1, (len(rows), expected + 1)
assert rows[0][:7] == [
    "schema_version", "interval", "duration_us", "timestamp", "timestamp_ns",
    "timestamp_iso", "Scope",
]
assert all(row[0] == "8" for row in rows[1:])
assert [int(row[1]) for row in rows[1:]] == list(range(1, expected + 1))
assert all(int(row[2]) > 0 for row in rows[1:])
assert all(int(row[4]) // 1_000_000_000 == int(row[3]) for row in rows[1:])
assert all(row[6] == "SUM" for row in rows[1:])
assert len({len(row) for row in rows}) == 1
assert all(cell != "-" for row in rows[1:] for cell in row)
PY

"$armstat_bin" -a -f csv -i "$sample_interval" -n 1 \
	>"$tmp_dir/mixed.csv"
python3 - "$tmp_dir/mixed.csv" <<'PY'
import csv
import sys

with open(sys.argv[1], newline="", encoding="utf-8") as stream:
    rows = list(csv.reader(stream))
assert len(rows) > 3
header = rows[0]
assert header[:9] == [
    "schema_version", "interval", "duration_us", "timestamp", "timestamp_ns",
    "timestamp_iso", "Scope", "CPU", "Package",
]
assert len({len(row) for row in rows}) == 1
scopes = {row[6] for row in rows[1:]}
assert {"SUM", "PKG", "CPU"}.issubset(scopes), scopes
for row in rows[1:]:
    assert row[0] == "8"
    assert int(row[2]) > 0
    if row[6] == "SUM":
        assert row[7] == "" and row[8] == ""
    elif row[6] == "PKG":
        assert row[7] == "" and row[8] != ""
    elif row[6] == "CPU":
        assert row[7] != "" and row[8] == ""
PY

"$armstat_bin" -s pkg_freq_mhz -f csv -i "$sample_interval" -n 1 \
	>"$tmp_dir/package.csv"
python3 - "$tmp_dir/package.csv" <<'PY'
import csv
import sys

with open(sys.argv[1], newline="", encoding="utf-8") as stream:
    rows = list(csv.reader(stream))
assert len(rows) > 1
assert rows[0][:8] == [
    "schema_version", "interval", "duration_us", "timestamp", "timestamp_ns",
    "timestamp_iso", "Package", "Freq",
]
assert len({len(row) for row in rows}) == 1
assert all(row[0] == "8" and int(row[2]) > 0 and row[6] != ""
           for row in rows[1:])
PY

"$armstat_bin" -s pkg_id -f json -i "$sample_interval" -n 1 \
	>"$tmp_dir/package-identity.json"
python3 - "$tmp_dir/package-identity.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    samples = json.load(stream)

assert len(samples) == 1
packages = samples[0].get("packages")
assert isinstance(packages, list) and packages
assert all(set(package) == {"package"} and isinstance(package["package"], int)
           for package in packages)
PY

for signal_name in INT TERM; do
	signal_json="$tmp_dir/signal_$signal_name.json"
	"$armstat_bin" -S -f json -i 2 -n 0 >"$signal_json" &
	child_pid=$!
	wait_for_sampling_ready "$child_pid"
	kill -"$signal_name" "$child_pid"
	wait "$child_pid"
	child_pid=
	python3 - "$signal_json" <<'PY'
import json
import sys

def reject_constant(value):
    raise ValueError(f"non-standard JSON constant: {value}")

with open(sys.argv[1], encoding="utf-8") as stream:
    assert json.load(stream, parse_constant=reject_constant) == []
PY
	done

if [ "$soak_iterations" -gt 0 ]; then
	"$armstat_bin" -S -a -f json -i "$soak_interval" \
		-n "$soak_iterations" >"$tmp_dir/soak.json" \
		2>"$tmp_dir/soak.stderr" &
	child_pid=$!
	peak_rss_kib=0
	peak_open_fds=0
	while kill -0 "$child_pid" 2>/dev/null; do
		current_rss_kib=$(awk '/^VmRSS:/ { print $2 }' \
			"/proc/$child_pid/status" 2>/dev/null || true)
		current_open_fds=$(find "/proc/$child_pid/fd" -mindepth 1 \
			-maxdepth 1 2>/dev/null | wc -l | tr -d ' ')
		case $current_rss_kib in
		''|*[!0-9]*)
			;;
		*)
			if [ "$current_rss_kib" -gt "$peak_rss_kib" ]; then
				peak_rss_kib=$current_rss_kib
			fi
			;;
		esac
		if [ "$current_open_fds" -gt "$peak_open_fds" ]; then
			peak_open_fds=$current_open_fds
		fi
		sleep 0.1
	done
	if ! wait "$child_pid"; then
		child_pid=
		echo "target-test soak capture failed" >&2
		exit 1
	fi
	child_pid=
	diagnostic_lines=$(wc -l <"$tmp_dir/soak.stderr" | tr -d ' ')
	if [ "$diagnostic_lines" -gt "$max_diagnostic_lines" ]; then
		echo "target-test soak diagnostics exceeded limit: $diagnostic_lines > $max_diagnostic_lines" >&2
		sed -n '1,20p' "$tmp_dir/soak.stderr" >&2
		exit 1
	fi
	if [ "$diagnostic_lines" -gt 0 ]; then
		echo "target-test: soak emitted $diagnostic_lines bounded diagnostic line(s):" >&2
		sed -n '1,20p' "$tmp_dir/soak.stderr" >&2
	fi
	if [ "$peak_rss_kib" -gt "$max_rss_kib" ]; then
		echo "target-test soak exceeded RSS limit: $peak_rss_kib KiB > $max_rss_kib KiB" >&2
		exit 1
	fi
	if [ "$peak_open_fds" -gt "$max_open_fds" ]; then
		echo "target-test soak exceeded fd limit: $peak_open_fds > $max_open_fds" >&2
		exit 1
	fi
	python3 - "$tmp_dir/soak.json" "$soak_iterations" <<'PY'
import json
import sys

def reject_constant(value):
    raise ValueError(f"non-standard JSON constant: {value}")

path, expected_text = sys.argv[1:]
with open(path, encoding="utf-8") as stream:
    samples = json.load(stream, parse_constant=reject_constant)
expected = int(expected_text)
assert len(samples) == expected, (len(samples), expected)
assert all(sample.get("schema_version") == 8 for sample in samples)
assert [sample.get("interval") for sample in samples] == list(range(1, expected + 1))
assert all(isinstance(sample.get("duration_us"), int) and
           sample["duration_us"] > 0 for sample in samples)
assert all(sample["timestamp_ns"] // 1_000_000_000 == sample["timestamp"]
           for sample in samples)
PY
	echo "target-test: soak peak RSS ${peak_rss_kib} KiB, peak fds ${peak_open_fds}"
fi

echo "target-test: ARM64 Linux runtime checks passed"
