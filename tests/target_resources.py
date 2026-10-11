#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Independently budget and monitor the ARM64 acceptance capture's resources."""

import argparse
from collections import Counter
import json
import math
import os
from pathlib import Path
import sys
import time

MAX_CPUS = 1024
VISIBLE_IDLE_STATES = 8
RUNTIME_FD_RESERVE = 64
MINIMUM_FD_LIMIT = 256
POLL_SECONDS = 0.1
INVENTORY_SECONDS = 1.0


class ResourceError(Exception):
    pass


def cpu_ids(text):
    """Parse sysfs CPU ranges, retaining IDs the monitor can represent."""
    result = set()
    for token in text.strip().split(","):
        bounds = token.split("-")
        if len(bounds) not in (1, 2) or any(not item.isdecimal() for item in bounds):
            raise ResourceError("invalid sysfs CPU list: " + repr(text))
        first, last = int(bounds[0]), int(bounds[-1])
        if first > last:
            raise ResourceError("reversed sysfs CPU range: " + token)
        result.update(range(first, min(last + 1, MAX_CPUS)))
    return sorted(result)


def existing_target(path):
    """Resolve optional sources accessible to the acceptance process."""
    try:
        return str(Path(path).resolve(strict=True))
    except (FileNotFoundError, PermissionError):
        return None


def directory_entries(path):
    try:
        return list(Path(path).iterdir())
    except (FileNotFoundError, PermissionError):
        return []


def discover_inventory(sys_root="/sys", proc_root="/proc", temp_policy=None):
    root = Path(sys_root)
    cpu_root = root / "devices/system/cpu"
    ids = cpu_ids((cpu_root / "present").read_text(encoding="ascii"))
    if not ids:
        raise ResourceError("no representable present CPUs for the FD budget")
    targets = Counter()
    counts = Counter()

    def add(path, kind):
        target = existing_target(path)
        if target is not None:
            targets[target] += 1
            counts[kind] += 1

    for cpu in ids:
        base = cpu_root / ("cpu" + str(cpu))
        add(base / "cpufreq/cpuinfo_cur_freq", "frequency")
        # The soak is -S -a: summary residency, without CPU usage columns.
        for state in range(VISIBLE_IDLE_STATES):
            add(base / ("cpuidle/state" + str(state)) / "time", "idle_time")
    add(Path(proc_root) / "stat", "proc")
    add(Path(proc_root) / "schedstat", "proc")
    if temp_policy is None:
        temp_policy = os.environ.get("ARMSTAT_TEMP_POLICY", "thermal-zone-index")
    if temp_policy not in ("none", "disabled"):
        for zone in range(16):
            if existing_target(root / ("devices/system/node/node" + str(zone))):
                add(root / ("class/thermal/thermal_zone" + str(zone)) / "temp", "temp")

    power = []
    for hwmon in directory_entries(root / "class/hwmon"):
        if not hwmon.name.startswith("hwmon"):
            continue
        try:
            name = (hwmon / "name").read_text(encoding="ascii").strip()
        except (FileNotFoundError, PermissionError):
            continue
        if name == "power_meter" and existing_target(hwmon / "power1_average"):
            power.append(hwmon / "power1_average")
    if len(power) == 1:
        add(power[0], "power")

    uncore = []
    keywords = ("uncore", "interconnect", "fabric", "noc", "cci", "ccn", "cmn")
    for device in directory_entries(root / "class/devfreq"):
        if any(word in device.name.lower() for word in keywords):
            if existing_target(device / "cur_freq"):
                uncore.append(device / "cur_freq")
    if len(uncore) == 1:
        add(uncore[0], "uncore")

    membw = []
    for device in directory_entries(root / "class/memory"):
        if device.name.startswith("mem") and existing_target(device / "mem_bytes_read"):
            membw.append(device / "mem_bytes_read")
    if len(membw) == 1:
        add(membw[0], "membw")
    return {"cpu_ids": ids, "targets": dict(targets), "counts": dict(counts),
            "pmu_fds": 2 * len(ids)}


def automatic_fd_limit(inventory, inherited_fds):
    # Never infer a larger allowance from observed descriptors or peak usage.
    # PMU initialization may raise the process's inherited soft RLIMIT_NOFILE.
    counts = inventory["counts"]
    return max(MINIMUM_FD_LIMIT, len(inherited_fds) + RUNTIME_FD_RESERVE +
               inventory["pmu_fds"] + counts.get("frequency", 0) +
               counts.get("idle_time", 0))


def fd_limit(inventory, inherited_fds, explicit=None):
    return explicit if explicit is not None else automatic_fd_limit(inventory, inherited_fds)


def residual_fd_count(fds, inventory, inherited_fds):
    allowances = Counter(inventory["targets"])
    perf_remaining = inventory["pmu_fds"]
    residual = 0
    for fd, target in sorted(fds.items(), key=lambda item: int(item[0])):
        if int(fd) in (1, 2):
            # The acceptance shell redirects stdout/stderr for the capture.
            continue
        if inherited_fds.get(str(fd)) == target:
            continue
        source_target = target.removesuffix(" (deleted)")
        if allowances[source_target] > 0:
            allowances[source_target] -= 1
        elif target == "anon_inode:[perf_event]" and perf_remaining > 0:
            perf_remaining -= 1
        else:
            residual += 1
    return residual


def inventory_with_grace(current, previous):
    """Allow independently discovered old sources briefly during rebuild."""
    targets = Counter(current["targets"])
    pmu_fds = current["pmu_fds"]
    for old in previous:
        targets |= Counter(old["targets"])
        pmu_fds = max(pmu_fds, old["pmu_fds"])
    return {**current, "targets": dict(targets), "pmu_fds": pmu_fds}


class GrowthTracker:
    """Use complete one-second window minima after one second of warmup."""

    def __init__(self):
        self.ready_at = None
        self.window_start = None
        self.window_min = None
        self.minima = []
        self.failure = None
        self.resets = 0
        self.previous_windows = 0
        self.previous_checked = False

    def reset(self):
        self.previous_windows += len(self.minima)
        self.previous_checked |= len(self.minima) >= 3
        self.resets += 1
        self.ready_at = self.window_start = self.window_min = None
        self.minima = []

    def _complete(self):
        self.minima.append(self.window_min)
        self.window_min = None
        if len(self.minima) < 3:
            return
        a, b, c = self.minima[-3:]
        baseline = self.minima[0]
        if a < b < c or all(value >= baseline + 3 for value in (a, b, c)):
            self.failure = ("unaccounted FD growth after warmup: window minima " +
                            repr(self.minima))

    def observe(self, now, residual, ready):
        if self.ready_at is None:
            if not ready:
                return
            self.ready_at = now
        if now - self.ready_at < 1.0:
            return
        if self.window_start is None:
            self.window_start = now
        elif now - self.window_start >= 1.0:
            self._complete()
            # A monitoring gap is not evidence for additional complete windows.
            self.window_start = now
        self.window_min = residual if self.window_min is None else min(self.window_min, residual)

    def finish(self, now):
        if (self.window_start is not None and self.window_min is not None and
                now - self.window_start >= 1.0):
            self._complete()
            self.window_start = None

    def result(self):
        return {"growth_checked": self.previous_checked or len(self.minima) >= 3,
                "growth_windows": self.previous_windows + len(self.minima),
                "stable_windows": len(self.minima), "inventory_resets": self.resets,
                "residual_window_minima": self.minima,
                "residual_baseline": self.minima[0] if self.minima else None}


def read_fd_targets(pid, proc_root="/proc"):
    directory = Path(proc_root) / str(pid) / "fd"
    result = {}
    # Directory permission errors fail. A missing process is handled by monitor.
    for path in directory.iterdir():
        if not path.name.isdecimal():
            continue
        try:
            result[path.name] = os.readlink(path)
        except FileNotFoundError:
            # A transient descriptor can close between enumeration and readlink.
            continue
    return result


def read_process(pid, proc_root="/proc"):
    # Linux can release mm before exposing the zombie state. Briefly re-read
    # this exit transition rather than treating missing RSS as zero usage.
    for attempt in range(3):
        try:
            text = (Path(proc_root) / str(pid) / "status").read_text(encoding="ascii")
        except FileNotFoundError:
            return None
        state = None
        rss = None
        for line in text.splitlines():
            if line.startswith("State:"):
                state = line.split()[1]
            elif line.startswith("VmRSS:"):
                rss = int(line.split()[1])
        if state is None:
            raise ResourceError("could not read monitored process state")
        if state in ("Z", "X", "x") or rss is not None:
            return state, rss
        if attempt < 2:
            time.sleep(0.01)
    raise ResourceError("could not read monitored process RSS")


def prepare(parent_pid, sys_root="/sys", proc_root="/proc"):
    inherited = read_fd_targets(parent_pid, proc_root)
    inventory = discover_inventory(sys_root, proc_root)
    return {"version": 1, "inherited_fds": inherited, "inventory": inventory,
            "automatic_limit": automatic_fd_limit(inventory, inherited)}


def monitor(pid, plan, max_rss_kib, max_open_fds=None, sys_root="/sys", proc_root="/proc",
            sample_interval=1.0):
    if plan.get("version") != 1:
        raise ResourceError("unsupported resource plan version")
    inherited = plan["inherited_fds"]
    inventory = plan["inventory"]
    tracker = GrowthTracker()
    peak_rss = peak_fds = observations = 0
    next_inventory = 0.0
    max_limit = fd_limit(inventory, inherited, max_open_fds)
    ceiling_inventory = inventory
    previous_inventories = []
    grace_seconds = max(1.0, 2 * sample_interval)

    def refresh_inventory(now):
        nonlocal inventory, next_inventory, max_limit, ceiling_inventory
        refreshed = discover_inventory(sys_root, proc_root)
        if refreshed != inventory:
            previous_inventories.append((now + grace_seconds, inventory))
            tracker.reset()
        inventory = refreshed
        next_inventory = now + INVENTORY_SECONDS
        limit = fd_limit(inventory, inherited, max_open_fds)
        if limit > max_limit:
            ceiling_inventory = inventory
        max_limit = max(max_limit, limit)

    while True:
        now = time.monotonic()
        process = read_process(pid, proc_root)
        if process is None or process[0] in ("Z", "X", "x"):
            tracker.finish(now)
            break
        inventory_refreshed = now >= next_inventory
        if inventory_refreshed:
            refresh_inventory(now)
        try:
            fds = read_fd_targets(pid, proc_root)
        except FileNotFoundError:
            process = read_process(pid, proc_root)
            if process is None or process[0] in ("Z", "X", "x"):
                tracker.finish(now)
                break
            raise ResourceError("monitored process FD directory disappeared")
        if not fds:
            process = read_process(pid, proc_root)
            if process is None or process[0] in ("Z", "X", "x"):
                tracker.finish(now)
                break
            raise ResourceError("could not observe any monitored process FDs")
        observations += 1
        rss = process[1]
        peak_rss = max(peak_rss, rss)
        peak_fds = max(peak_fds, len(fds))
        if rss > max_rss_kib:
            raise ResourceError(f"soak exceeded RSS limit: {rss} KiB > {max_rss_kib} KiB")
        previous_inventories[:] = [(end, old) for end, old in previous_inventories if end > now]
        accounting_inventory = inventory_with_grace(inventory, [old for _, old in previous_inventories])
        residual = residual_fd_count(fds, accounting_inventory, inherited)
        # A source may recover in between the periodic inventory scans. Before
        # rejecting a burst, verify demand independently once more; descriptors
        # themselves never become an allowance or a larger hard limit.
        if len(fds) > max_limit or residual > RUNTIME_FD_RESERVE:
            refresh_inventory(now)
            accounting_inventory = inventory_with_grace(inventory, [old for _, old in previous_inventories])
            residual = residual_fd_count(fds, accounting_inventory, inherited)
        if len(fds) > max_limit:
            raise ResourceError(f"soak exceeded FD limit: {len(fds)} > {max_limit}")
        if residual > RUNTIME_FD_RESERVE:
            raise ResourceError(f"soak exceeded unaccounted FD reserve: "
                                f"{residual} > {RUNTIME_FD_RESERVE}")
        tracker.observe(now, residual, str(Path(proc_root) / "stat") in fds.values())
        if tracker.failure:
            raise ResourceError(tracker.failure)
        time.sleep(POLL_SECONDS)
    if not observations:
        raise ResourceError("soak exited before its resource usage could be observed")
    if tracker.failure:
        raise ResourceError(tracker.failure)
    result = {"peak_rss_kib": peak_rss, "peak_open_fds": peak_fds,
              "max_open_fds": max_limit, "fd_limit_explicit": max_open_fds is not None,
              "observations": observations, "ceiling_inventory": ceiling_inventory,
              "inherited_fds": len(inherited), **tracker.result()}
    if not result["growth_checked"]:
        result["growth_note"] = "not checked: fewer than three complete post-warmup windows"
    return result


def print_monitor_result(result):
    inventory = result["ceiling_inventory"]
    counts = inventory["counts"]
    if result["fd_limit_explicit"]:
        detail = "explicit ARMSTAT_MAX_OPEN_FDS"
    else:
        detail = (f"auto: CPUs={len(inventory['cpu_ids'])}, "
                  f"current={counts.get('frequency', 0)}, LPI={counts.get('idle_time', 0)}, "
                  f"perf={inventory['pmu_fds']}, inherited={result['inherited_fds']}, "
                  f"reserve={RUNTIME_FD_RESERVE}")
    print(f"target-test: soak peak RSS {result['peak_rss_kib']} KiB, "
          f"peak fds {result['peak_open_fds']} / ceiling {result['max_open_fds']} ({detail})")
    if result["growth_checked"]:
        print(f"target-test: FD growth checked over {result['growth_windows']} complete windows; "
              f"inventory resets {result['inventory_resets']}")
    else:
        print("target-test: FD growth not checked: fewer than three complete windows "
              "in one stable inventory period "
              f"(windows={result['growth_windows']}, inventory resets={result['inventory_resets']})")


def positive_integer(value):
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def positive_interval(value):
    parsed = float(value)
    if parsed <= 0 or not math.isfinite(parsed):
        raise argparse.ArgumentTypeError("must be a finite positive interval")
    return parsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    prepare_parser = actions.add_parser("prepare")
    prepare_parser.add_argument("--parent", type=positive_integer, required=True)
    prepare_parser.add_argument("--sys-root", default="/sys")
    prepare_parser.add_argument("--proc-root", default="/proc")
    monitor_parser = actions.add_parser("monitor")
    monitor_parser.add_argument("--pid", type=positive_integer, required=True)
    monitor_parser.add_argument("--plan", type=Path, required=True)
    monitor_parser.add_argument("--max-rss-kib", type=positive_integer, required=True)
    monitor_parser.add_argument("--max-open-fds", type=positive_integer)
    monitor_parser.add_argument("--sample-interval", type=positive_interval, default=1.0)
    monitor_parser.add_argument("--sys-root", default="/sys")
    monitor_parser.add_argument("--proc-root", default="/proc")
    args = parser.parse_args()
    try:
        if args.action == "prepare":
            result = prepare(args.parent, args.sys_root, args.proc_root)
            print(json.dumps(result, sort_keys=True))
        else:
            with args.plan.open(encoding="utf-8") as stream:
                plan = json.load(stream)
            result = monitor(args.pid, plan, args.max_rss_kib, args.max_open_fds,
                             args.sys_root, args.proc_root, args.sample_interval)
            print_monitor_result(result)
    except (ResourceError, OSError, ValueError, KeyError, TypeError) as error:
        print("target-test resources: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
