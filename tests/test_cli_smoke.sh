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
grep -F -- "--num-iterations <count>" "$tmp_dir/help.txt" >/dev/null
grep -F -- "0 = unlimited" "$tmp_dir/help.txt" >/dev/null
"$armstat_bin" -l >"$tmp_dir/list.txt" 2>"$tmp_dir/list.stderr"
test ! -s "$tmp_dir/list.stderr"
grep -F -- "Exact fields (stable ID" "$tmp_dir/list.txt" >/dev/null
grep -F -- "avg_mhz" "$tmp_dir/list.txt" >/dev/null
grep -F -- "cpu_temp_c" "$tmp_dir/list.txt" >/dev/null
grep -F -- "unit=MHz" "$tmp_dir/list.txt" >/dev/null
grep -F -- "unit=MiB/s" "$tmp_dir/list.txt" >/dev/null
grep -F -- "type=boolean" "$tmp_dir/list.txt" >/dev/null
"$armstat_bin" -v >"$tmp_dir/version.txt"
grep -Fx "armstat version $(sed -n '1p' "$src_dir/VERSION")" \
	"$tmp_dir/version.txt" >/dev/null

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

# A platform-init failure must not truncate a previous valid capture.
printf '%s\n' 'previous-valid-export' >"$tmp_dir/existing.out"
if "$armstat_bin" --probe -o "$tmp_dir/existing.out" >/dev/null 2>&1; then
	:
else
	grep -Fx 'previous-valid-export' "$tmp_dir/existing.out" >/dev/null
fi
