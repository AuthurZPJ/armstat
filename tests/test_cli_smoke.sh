#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

src_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
armstat_bin=${ARMSTAT_BIN:-./armstat}
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/armstat-cli.XXXXXX")

cleanup()
{
	rm -rf "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

"$armstat_bin" --help >"$tmp_dir/help.txt"
"$armstat_bin" -h >/dev/null
grep -F -- "--num_iterations <count>" "$tmp_dir/help.txt" >/dev/null
grep -F -- "--header_iterations <count>" "$tmp_dir/help.txt" >/dev/null
grep -F -- "--Summary" "$tmp_dir/help.txt" >/dev/null
grep -F -- "--Joules" "$tmp_dir/help.txt" >/dev/null
grep -F -- "--out <file>" "$tmp_dir/help.txt" >/dev/null
grep -F -- "--Dump" "$tmp_dir/help.txt" >/dev/null
grep -F -- "--no-perf" "$tmp_dir/help.txt" >/dev/null
grep -F -- "--quiet-all" "$tmp_dir/help.txt" >/dev/null
grep -F -- "0 = unlimited" "$tmp_dir/help.txt" >/dev/null
grep -F -- "text, json, ndjson, csv" "$tmp_dir/help.txt" >/dev/null
"$armstat_bin" -f ndjson -l >"$tmp_dir/ndjson-list.txt" \
	2>"$tmp_dir/ndjson-list.stderr"
test ! -s "$tmp_dir/ndjson-list.stderr"
"$armstat_bin" -l >"$tmp_dir/list.txt" 2>"$tmp_dir/list.stderr"
test ! -s "$tmp_dir/list.stderr"
grep -F -- "Exact fields (stable ID" "$tmp_dir/list.txt" >/dev/null
grep -F -- "summary_freq_mhz" "$tmp_dir/list.txt" >/dev/null
grep -F -- "cpu_temp_c" "$tmp_dir/list.txt" >/dev/null
grep -F -- "unit=MHz" "$tmp_dir/list.txt" >/dev/null
grep -F -- "unit=MiB/s" "$tmp_dir/list.txt" >/dev/null
grep -F -- "type=boolean" "$tmp_dir/list.txt" >/dev/null
"$armstat_bin" -v >"$tmp_dir/version.txt"
grep -Fx "armstat version $(sed -n '1p' "$src_dir/VERSION")" \
	"$tmp_dir/version.txt" >/dev/null

expect_cli_error()
{
	if "$armstat_bin" "$@" >"$tmp_dir/cli.stdout" 2>"$tmp_dir/cli.stderr"; then
		echo "invalid CLI unexpectedly succeeded: $*" >&2
		exit 1
	else
		cli_ec=$?
	fi
	test "$cli_ec" -eq 1
}

# Canonical long options parse before the platform-independent list exit.
"$armstat_bin" --num_iterations 0 --header_iterations 0 --Summary --Joules \
	--out "$tmp_dir/not-opened.out" --no-perf -l >"$tmp_dir/canonical.txt" \
	2>"$tmp_dir/canonical.stderr"
test ! -s "$tmp_dir/canonical.stderr"
test ! -e "$tmp_dir/not-opened.out"
grep -F -- "summary_freq_mhz" "$tmp_dir/canonical.txt" >/dev/null
for removed in --num-iterations --header-iterations --output --export -O; do
	expect_cli_error "$removed" 1
done
for removed in --summary --joules --dump; do
	expect_cli_error "$removed"
done
expect_cli_error -P -I
grep -F -- "--no-perf cannot be combined" "$tmp_dir/cli.stderr" >/dev/null
expect_cli_error -I -P
expect_cli_error -D -f json
grep -F -- "--Dump requires text" "$tmp_dir/cli.stderr" >/dev/null
expect_cli_error -D -f ndjson
grep -F -- "--Dump requires text" "$tmp_dir/cli.stderr" >/dev/null
expect_cli_error --
grep -F -- "-- requires a command" "$tmp_dir/cli.stderr" >/dev/null
expect_cli_error true -n 1
grep -F -- "Use -- before a command" "$tmp_dir/cli.stderr" >/dev/null
expect_cli_error -i 1 -- true
grep -F -- "command mode cannot be combined" "$tmp_dir/cli.stderr" >/dev/null

if "$armstat_bin" --definitely-not-an-armstat-option >/dev/null 2>&1; then
	echo "unknown option unexpectedly succeeded" >&2
	exit 1
fi

if "$armstat_bin" -i not-a-number >/dev/null 2>&1; then
	echo "invalid interval unexpectedly succeeded" >&2
	exit 1
fi

if "$armstat_bin" -i 0 >/dev/null 2>&1; then
	echo "zero interval unexpectedly succeeded" >&2
	exit 1
fi

if "$armstat_bin" -n -1 >/dev/null 2>&1; then
	echo "negative iteration count unexpectedly succeeded" >&2
	exit 1
fi

if "$armstat_bin" -N not-a-number >/dev/null 2>&1; then
	echo "invalid header interval unexpectedly succeeded" >&2
	exit 1
fi

if "$armstat_bin" -N -1 >/dev/null 2>&1; then
	echo "negative header interval unexpectedly succeeded" >&2
	exit 1
fi

if "$armstat_bin" -f xml >/dev/null 2>&1; then
	echo "unknown output format unexpectedly succeeded" >&2
	exit 1
fi

if "$armstat_bin" --busy-source mystery >/dev/null 2>&1; then
	echo "unknown busy source unexpectedly succeeded" >&2
	exit 1
fi

# --probe must not crash regardless of platform; exit 0 (success) or 1
# (init failure on non-ARM) are both acceptable, but signals are not.
if "$armstat_bin" --probe >/dev/null 2>&1; then
	probe_ec=0
else
	probe_ec=$?
fi
if [ "$probe_ec" -ne 0 ] && [ "$probe_ec" -ne 1 ]; then
	echo "--probe exited with unexpected code $probe_ec" >&2
	exit 1
fi

# The no-perf capability probe must skip perf rather than act as old -P.
if "$armstat_bin" -P --probe >"$tmp_dir/no-perf-probe.out" \
	2>"$tmp_dir/no-perf-probe.stderr"; then
	grep -F -- "disabled by --no-perf; perf was not probed" \
		"$tmp_dir/no-perf-probe.out" >/dev/null
else
	no_perf_probe_ec=$?
	test "$no_perf_probe_ec" -eq 1
fi

# A platform-init failure must not truncate a previous valid capture.
printf '%s\n' 'previous-valid-export' >"$tmp_dir/existing.out"
if "$armstat_bin" --probe -o "$tmp_dir/existing.out" >/dev/null 2>&1; then
	:
else
	grep -Fx 'previous-valid-export' "$tmp_dir/existing.out" >/dev/null
fi
