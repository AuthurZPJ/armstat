#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

./armstat --help >/dev/null
./armstat -l >/dev/null
./armstat -v >/dev/null

if ./armstat --definitely-not-an-armstat-option >/dev/null 2>&1; then
	echo "unknown option unexpectedly succeeded" >&2
	exit 1
fi

if ./armstat -i not-a-number >/dev/null 2>&1; then
	echo "invalid interval unexpectedly succeeded" >&2
	exit 1
fi

if ./armstat -i 0 >/dev/null 2>&1; then
	echo "zero interval unexpectedly succeeded" >&2
	exit 1
fi

if ./armstat -n -1 >/dev/null 2>&1; then
	echo "negative iteration count unexpectedly succeeded" >&2
	exit 1
fi

if ./armstat -N not-a-number >/dev/null 2>&1; then
	echo "invalid header interval unexpectedly succeeded" >&2
	exit 1
fi

if ./armstat -f xml >/dev/null 2>&1; then
	echo "unknown output format unexpectedly succeeded" >&2
	exit 1
fi

if ./armstat --busy-source mystery >/dev/null 2>&1; then
	echo "unknown busy source unexpectedly succeeded" >&2
	exit 1
fi
