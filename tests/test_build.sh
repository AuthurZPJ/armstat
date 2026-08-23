#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

# This script verifies armstat's own defaults. Top-level `make test` may have
# been invoked with sanitizer, coverage, cross-build, or packaging overrides;
# do not let those values silently redefine the release baseline below.
unset MAKEFLAGS MFLAGS MAKEOVERRIDES CC CFLAGS CPPFLAGS LDFLAGS LDLIBS
unset CROSS_COMPILE O

src_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/armstat-build.XXXXXX")
build_dir="$tmp_dir/build"
stage_dir="$tmp_dir/stage"

cleanup()
{
	rm -rf "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

MAKEFLAGS= MFLAGS= MAKEOVERRIDES= make -s -j4 -C "$src_dir" O="$build_dir" all

test -x "$build_dir/armstat"
test -d "$build_dir/.armstat-obj"
test -f "$build_dir/.armstat-build-config"
grep '^CC_MACHINE=' "$build_dir/.armstat-build-config" >/dev/null
grep '^CC_VERSION=' "$build_dir/.armstat-build-config" >/dev/null
grep -Fx "VERSION=$(sed -n '1p' "$src_dir/VERSION")" \
	"$build_dir/.armstat-build-config" >/dev/null
"$build_dir/armstat" --version >/dev/null

if [ "$(uname -s)" = Linux ] && command -v readelf >/dev/null 2>&1; then
	readelf -h "$build_dir/armstat" | grep 'Type:.*DYN' >/dev/null
	readelf -l "$build_dir/armstat" | grep 'GNU_RELRO' >/dev/null
	readelf -d "$build_dir/armstat" | grep 'BIND_NOW' >/dev/null
fi

# Test executables also use stable public paths. Verify that changing build
# flags cannot silently reuse a binary linked against another object set.
MAKEFLAGS= MFLAGS= MAKEOVERRIDES= make -s -j4 -C "$src_dir" O="$build_dir" \
	tests/test_runtime_smoke
release_test_cksum=$(cksum "$build_dir/tests/test_runtime_smoke")

release_binary_cksum=$(cksum "$build_dir/armstat")
MAKEFLAGS= MFLAGS= MAKEOVERRIDES= make -s -j4 -C "$src_dir" O="$build_dir" \
	CFLAGS="-O0 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -MMD -MP" \
	LDFLAGS= \
	all
debug_binary_cksum=$(cksum "$build_dir/armstat")
test "$release_binary_cksum" != "$debug_binary_cksum"
MAKEFLAGS= MFLAGS= MAKEOVERRIDES= make -s -j4 -C "$src_dir" O="$build_dir" \
	CFLAGS="-O0 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -MMD -MP" \
	LDFLAGS= \
	tests/test_runtime_smoke
debug_test_cksum=$(cksum "$build_dir/tests/test_runtime_smoke")
test "$release_test_cksum" != "$debug_test_cksum"

# Returning to the default build must not reuse the differently configured objects.
MAKEFLAGS= MFLAGS= MAKEOVERRIDES= make -s -j4 -C "$src_dir" O="$build_dir" all
test "$release_binary_cksum" = "$(cksum "$build_dir/armstat")"
"$build_dir/armstat" --version >/dev/null
MAKEFLAGS= MFLAGS= MAKEOVERRIDES= make -s -j4 -C "$src_dir" O="$build_dir" \
	tests/test_runtime_smoke
test "$release_test_cksum" = "$(cksum "$build_dir/tests/test_runtime_smoke")"

MAKEFLAGS= MFLAGS= MAKEOVERRIDES= make -s -j4 -C "$src_dir" O="$build_dir" \
	DESTDIR="$stage_dir" PREFIX=/usr install

test -x "$stage_dir/usr/bin/armstat"
test -x "$stage_dir/usr/bin/armstat-plot-summary"
test -x "$stage_dir/usr/bin/armstat-plot-cpu"
test -r "$stage_dir/usr/share/armstat/plot_utils.py"
test -r "$stage_dir/usr/share/armstat/armstat_loader.py"
test -r "$stage_dir/usr/share/man/man8/armstat.8"
test -r "$stage_dir/usr/share/doc/armstat/COPYING"
test -r "$stage_dir/usr/share/doc/armstat/VERSION"
test -r "$stage_dir/usr/share/doc/armstat/README.md"
test -r "$stage_dir/usr/share/doc/armstat/README.zh-CN.md"
test -r "$stage_dir/usr/share/doc/armstat/docs/REFERENCE.md"
test -r "$stage_dir/usr/share/doc/armstat/docs/REFERENCE.zh-CN.md"
"$stage_dir/usr/bin/armstat-plot-summary" --help >/dev/null
"$stage_dir/usr/bin/armstat-plot-cpu" --help >/dev/null

MAKEFLAGS= MFLAGS= MAKEOVERRIDES= make -s -j4 -C "$src_dir" O="$build_dir" \
	DESTDIR="$stage_dir" PREFIX=/usr uninstall

test ! -e "$stage_dir/usr/bin/armstat"
test ! -e "$stage_dir/usr/bin/armstat-plot-summary"
test ! -e "$stage_dir/usr/bin/armstat-plot-cpu"
test ! -e "$stage_dir/usr/share/armstat"
test ! -e "$stage_dir/usr/share/man/man8/armstat.8"
test ! -e "$stage_dir/usr/share/doc/armstat"

# Coverage builds emit compiler data beside their targets. Keep `make clean`
# trustworthy for both in-tree and out-of-tree builds.
: >"$build_dir/coverage.gcda"
: >"$build_dir/tests/coverage.gcno"
MAKEFLAGS= MFLAGS= MAKEOVERRIDES= make -s -C "$src_dir" O="$build_dir" clean
test ! -e "$build_dir/coverage.gcda"
test ! -e "$build_dir/tests/coverage.gcno"
