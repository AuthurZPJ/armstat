#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Hardware-independent regressions for ARM64 target resource acceptance."""

from collections import Counter
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import target_resources as resources


def inventory(cpus=128, states=2, shared_frequency=False):
    targets = Counter({"/proc/stat": 1, "/proc/schedstat": 1})
    for cpu in range(cpus):
        target = "/policy0/cpuinfo_cur_freq" if shared_frequency else f"/cpu{cpu}/frequency"
        targets[target] += 1
        for state in range(states):
            targets[f"/cpu{cpu}/state{state}/time"] += 1
    return {"cpu_ids": list(range(cpus)), "targets": dict(targets),
            "counts": {"frequency": cpus, "idle_time": cpus * states, "proc": 2},
            "pmu_fds": cpus * 2}


def logical_fds(source_inventory):
    fds = {"0": "/dev/null", "1": "/tmp/capture.json", "2": "/tmp/capture.stderr"}
    next_fd = 3
    for target, count in source_inventory["targets"].items():
        for _ in range(count):
            fds[str(next_fd)] = target
            next_fd += 1
    for _ in range(source_inventory["pmu_fds"]):
        fds[str(next_fd)] = "anon_inode:[perf_event]"
        next_fd += 1
    return fds


class Clock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now = round(self.now + seconds, 7)


def run_monitor(initial, duration=5.2, fd_callback=None, inventory_callback=None,
                explicit=None, inherited=None, rss=4096):
    clock = Clock()
    if inherited is None:
        inherited = {"0": "/dev/null", "1": "/parent/stdout", "2": "/parent/stderr"}
    plan = {"version": 1, "inventory": initial, "inherited_fds": inherited}
    fd_callback = fd_callback or (lambda now: logical_fds(initial))
    inventory_callback = inventory_callback or (lambda now: initial)
    with contextlib.ExitStack() as stack:
        stack.enter_context(patch.object(resources.time, "monotonic", clock.monotonic))
        stack.enter_context(patch.object(resources.time, "sleep", clock.sleep))
        stack.enter_context(patch.object(resources, "read_process",
                                       side_effect=lambda *args: ("S", rss) if clock.now < duration
                                       else ("Z", None)))
        stack.enter_context(patch.object(resources, "read_fd_targets",
                                       side_effect=lambda *args: fd_callback(clock.now)))
        stack.enter_context(patch.object(resources, "discover_inventory",
                                       side_effect=lambda *args: inventory_callback(clock.now)))
        return resources.monitor(123, plan, 16384, explicit)


class BudgetTests(unittest.TestCase):
    def test_large_cpu_caches_are_included(self):
        inherited = {"0": "stdin", "1": "stdout", "2": "stderr"}
        for cpus in (128, 256, 1024):
            for states in (2, 8):
                with self.subTest(cpus=cpus, states=states):
                    source_inventory = inventory(cpus, states)
                    expected = 3 + 64 + cpus * (3 + states)
                    self.assertEqual(resources.automatic_fd_limit(source_inventory, inherited), expected)
                    self.assertLessEqual(len(logical_fds(source_inventory)), expected)
                    self.assertGreater(len(logical_fds(source_inventory)), max(256, 2 * cpus + 64))

    def test_shared_frequency_policy_preserves_cpu_multiplicity(self):
        source_inventory = inventory(128, 2, shared_frequency=True)
        self.assertEqual(source_inventory["targets"]["/policy0/cpuinfo_cur_freq"], 128)
        fds = logical_fds(source_inventory)
        self.assertEqual(resources.residual_fd_count(fds, source_inventory, {"0": "/dev/null"}), 0)
        fds["9000"] = "/policy0/cpuinfo_cur_freq"
        self.assertEqual(resources.residual_fd_count(fds, source_inventory, {"0": "/dev/null"}), 1)

    def test_explicit_override_is_never_raised(self):
        self.assertEqual(resources.fd_limit(inventory(1024, 8), {}, 10), 10)
        with self.assertRaisesRegex(resources.ResourceError, "FD limit"):
            run_monitor(inventory(128, 2), explicit=10)

    def test_budget_does_not_assume_inherited_soft_rlimit_is_final(self):
        # A PMU can raise a low soft limit under a higher hard limit. Neither
        # limit participates in the independent demand calculation.
        source_inventory = inventory(256, 8)
        low_soft, high_hard = 128, 4096
        limit = resources.automatic_fd_limit(source_inventory, {})
        self.assertGreater(limit, low_soft)
        self.assertLess(limit, high_hard)

    def test_stdio_redirection_and_other_inherited_fds(self):
        source_inventory = inventory(1, 0)
        fds = logical_fds(source_inventory)
        fds["40"] = "pipe:[1234]"
        inherited = {"0": "/dev/null", "1": "old stdout", "2": "old stderr",
                     "40": "pipe:[1234]"}
        self.assertEqual(resources.residual_fd_count(fds, source_inventory, inherited), 0)
        fds["41"] = "pipe:[1234]"
        self.assertEqual(resources.residual_fd_count(fds, source_inventory, inherited), 1)
        fds["40"] = "pipe:[changed]"
        self.assertEqual(resources.residual_fd_count(fds, source_inventory, inherited), 2)

    def test_fd_zero_can_be_a_cache_and_consumes_its_allowance(self):
        source_inventory = inventory(1, 0)
        fds = {"0": "/cpu0/frequency", "1": "stdout", "2": "stderr",
               "3": "/cpu0/frequency"}
        self.assertEqual(resources.residual_fd_count(fds, source_inventory, {}), 1)
        fds["0"] = "pipe:[unaccounted]"
        self.assertEqual(resources.residual_fd_count(fds, source_inventory, {}), 1)


class InventoryTests(unittest.TestCase):
    def test_sparse_heterogeneous_states_and_policy_aliases(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            cpu_root = base / "sys/devices/system/cpu"
            cpu_root.mkdir(parents=True)
            (cpu_root / "present").write_text("0,128,1023-1025\n")
            policy = cpu_root / "cpufreq/policy0"
            policy.mkdir(parents=True)
            (policy / "cpuinfo_cur_freq").touch()
            for cpu, states in ((0, (0, 2, 8, 15)), (128, (0,)), (1023, (0, 1))):
                cpu_dir = cpu_root / f"cpu{cpu}"
                cpu_dir.mkdir()
                (cpu_dir / "cpufreq").symlink_to(policy, target_is_directory=True)
                for state in states:
                    state_dir = cpu_dir / f"cpuidle/state{state}"
                    state_dir.mkdir(parents=True)
                    (state_dir / "time").touch()
                    (state_dir / "usage").touch()
            proc = base / "proc"
            proc.mkdir()
            (proc / "stat").touch()
            (proc / "schedstat").touch()
            result = resources.discover_inventory(base / "sys", proc)
            self.assertEqual(result["cpu_ids"], [0, 128, 1023])
            self.assertEqual(result["counts"]["frequency"], 3)
            self.assertEqual(result["counts"]["idle_time"], 5)
            self.assertEqual(result["targets"][str((policy / "cpuinfo_cur_freq").resolve())], 3)
            self.assertFalse(any("usage" in target for target in result["targets"]))
            self.assertFalse(any("state8" in target or "state15" in target
                                 for target in result["targets"]))

    def test_cpu_list_validation_and_representation(self):
        self.assertEqual(resources.cpu_ids("0-2,128,1023-1025,2048"), [0, 1, 2, 128, 1023])
        for invalid in ("", "2-1", "-1", "0,,1", "not-a-cpu", "0-1-2"):
            with self.subTest(invalid=invalid):
                with self.assertRaises(resources.ResourceError):
                    resources.cpu_ids(invalid)


class GrowthTests(unittest.TestCase):
    def test_normal_capture_and_short_observation(self):
        result = run_monitor(inventory(128, 2))
        self.assertTrue(result["growth_checked"])
        self.assertEqual(result["residual_window_minima"], [0, 0, 0, 0])
        result = run_monitor(inventory(1, 0), duration=0.3)
        self.assertFalse(result["growth_checked"])
        self.assertEqual(result["growth_windows"], 0)
        self.assertIn("not checked", result["growth_note"])

    def test_noncache_fd_growth_fails(self):
        source_inventory = inventory(1, 0)

        def fds(now):
            result = logical_fds(source_inventory)
            for extra in range(max(0, int(now) - 1)):
                result[str(100 + extra)] = f"pipe:[leak{extra}]"
            return result

        with self.assertRaisesRegex(resources.ResourceError, "unaccounted FD growth"):
            run_monitor(source_inventory, fd_callback=fds)

    def test_duplicate_cache_fd_growth_fails(self):
        source_inventory = inventory(1, 0)

        def fds(now):
            result = logical_fds(source_inventory)
            for extra in range(max(0, int(now) - 1)):
                result[str(100 + extra)] = "/cpu0/frequency"
            return result

        with self.assertRaisesRegex(resources.ResourceError, "unaccounted FD growth"):
            run_monitor(source_inventory, fd_callback=fds)

    def test_startup_or_warmup_burst_cannot_use_unused_pmu_allowance(self):
        source_inventory = inventory(128, 0)
        ordinary = {"0": "/dev/null", "1": "capture", "2": "diagnostics", "3": "/proc/stat"}

        def fds(now, start):
            result = dict(ordinary)
            if now >= start:
                result.update({str(100 + extra): "/dev/null" for extra in range(70)})
            return result

        for start in (0, 0.4):
            with self.subTest(start=start):
                with self.assertRaisesRegex(resources.ResourceError, "unaccounted FD reserve"):
                    run_monitor(source_inventory, fd_callback=lambda now: fds(now, start))

    def test_large_legitimate_recovery_between_inventory_scans_is_rechecked(self):
        original = inventory(128, 0)
        recovered = inventory(128, 2)

        def current(now):
            return original if now < 0.3 else recovered

        result = run_monitor(original, fd_callback=lambda now: logical_fds(current(now)),
                             inventory_callback=current)
        self.assertTrue(result["growth_checked"])
        self.assertEqual(result["inventory_resets"], 1)
        self.assertTrue(all(value == 0 for value in result["residual_window_minima"]))

    def test_recovery_after_scan_before_fd_snapshot_is_rechecked(self):
        original = inventory(128, 0)
        recovered = inventory(128, 2)
        scans = 0

        def current(now):
            nonlocal scans
            scans += 1
            return original if scans == 1 else recovered

        result = run_monitor(original, fd_callback=lambda now: logical_fds(recovered),
                             inventory_callback=current)
        self.assertTrue(result["growth_checked"])
        self.assertEqual(result["inventory_resets"], 1)
        self.assertEqual(result["residual_baseline"], 0)

    def test_persistent_jump_of_three_fds_fails(self):
        source_inventory = inventory(1, 0)

        def fds(now):
            result = logical_fds(source_inventory)
            if now >= 2.1:
                result.update({str(100 + extra): f"pipe:[leak{extra}]" for extra in range(3)})
            return result

        with self.assertRaisesRegex(resources.ResourceError, "unaccounted FD growth"):
            run_monitor(source_inventory, duration=6.2, fd_callback=fds)

    def test_short_configuration_read_spikes_do_not_fail(self):
        source_inventory = inventory(1, 0)

        def fds(now):
            result = logical_fds(source_inventory)
            if round(now * 10) % 10 == 5:
                result.update({str(100 + extra): f"/cpu0/config{extra}" for extra in range(5)})
            return result

        result = run_monitor(source_inventory, fd_callback=fds)
        self.assertTrue(result["growth_checked"])
        self.assertEqual(result["residual_window_minima"], [0, 0, 0, 0])

    def test_recovered_source_and_hotplug_reset_stable_windows(self):
        original = inventory(1, 0)
        recovered = inventory(1, 2)
        hotplugged = inventory(128, 2)

        def current(now):
            return original if now < 2 else recovered if now < 4 else hotplugged

        result = run_monitor(original, duration=9.2,
                             fd_callback=lambda now: logical_fds(current(now)),
                             inventory_callback=current)
        self.assertTrue(result["growth_checked"])
        self.assertEqual(result["inventory_resets"], 2)
        self.assertEqual(result["max_open_fds"], resources.automatic_fd_limit(hotplugged, {"0": "", "1": "", "2": ""}))
        self.assertTrue(all(value == 0 for value in result["residual_window_minima"]))

    def test_independent_ceiling_does_not_shrink_on_removal(self):
        original = inventory(128, 2)
        removed = inventory(1, 0)
        result = run_monitor(original, duration=0.3,
                             inventory_callback=lambda now: removed)
        self.assertEqual(result["max_open_fds"], resources.automatic_fd_limit(original, {"0": "", "1": "", "2": ""}))
        self.assertEqual(result["inventory_resets"], 1)

    def test_removed_sources_have_bounded_rebuild_grace(self):
        original = inventory(128, 2)
        removed = inventory(1, 0)

        def fds(now):
            source_inventory = original if now < 1.5 else removed
            result = logical_fds(source_inventory)
            if now < 1.5:
                result = {fd: target + " (deleted)" if target.startswith("/cpu") else target
                          for fd, target in result.items()}
            return result

        result = run_monitor(original, duration=5.2, fd_callback=fds,
                             inventory_callback=lambda now: removed)
        self.assertTrue(result["growth_checked"])
        with self.assertRaisesRegex(resources.ResourceError, "unaccounted FD reserve"):
            run_monitor(original, duration=5.2, inventory_callback=lambda now: removed)

    def test_exit_before_observation_and_empty_fd_view_fail(self):
        with self.assertRaisesRegex(resources.ResourceError, "before.*observed"):
            run_monitor(inventory(1, 0), duration=0)
        with self.assertRaisesRegex(resources.ResourceError, "any.*FDs"):
            run_monitor(inventory(1, 0), fd_callback=lambda now: {})

    def test_rss_threshold_and_resource_read_errors_fail(self):
        with self.assertRaisesRegex(resources.ResourceError, "RSS limit"):
            run_monitor(inventory(1, 0), rss=20000)
        with patch.object(Path, "iterdir", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                resources.read_fd_targets(123)
        with patch.object(Path, "resolve", side_effect=PermissionError("denied")):
            self.assertIsNone(resources.existing_target("/sys/denied"))
        with patch.object(Path, "read_text", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                resources.read_process(123)

    def test_exit_transition_without_rss_is_rechecked(self):
        with patch.object(Path, "read_text", side_effect=["State:\tR (running)\n",
                                                        "State:\tZ (zombie)\n"]):
            with patch.object(resources.time, "sleep"):
                self.assertEqual(resources.read_process(123), ("Z", None))
        with patch.object(Path, "read_text", side_effect=["State:\tD (disk sleep)\n",
                                                        "State:\tS (sleeping)\nVmRSS:\t123 kB\n"]):
            with patch.object(resources.time, "sleep"):
                self.assertEqual(resources.read_process(123), ("S", 123))
        with patch.object(Path, "read_text", return_value="State:\tR (running)\n"):
            with patch.object(resources.time, "sleep"):
                with self.assertRaisesRegex(resources.ResourceError, "RSS"):
                    resources.read_process(123)
        for state in ("X", "x"):
            with patch.object(Path, "read_text", return_value=f"State:\t{state} (dead)\n"):
                self.assertEqual(resources.read_process(123), (state, None))

    def test_fd_readlink_race_does_not_hide_permission_errors(self):
        paths = [Path("/proc/123/fd/3"), Path("/proc/123/fd/4")]
        with patch.object(Path, "iterdir", return_value=iter(paths)):
            with patch.object(resources.os, "readlink", side_effect=[FileNotFoundError(), "/proc/stat"]):
                self.assertEqual(resources.read_fd_targets(123), {"4": "/proc/stat"})
        with patch.object(Path, "iterdir", return_value=iter(paths)):
            with patch.object(resources.os, "readlink", side_effect=PermissionError("denied")):
                with self.assertRaises(PermissionError):
                    resources.read_fd_targets(123)

    def test_human_report_explains_budget_and_unchecked_growth(self):
        result = run_monitor(inventory(128, 2), duration=0.3)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            resources.print_monitor_result(result)
        self.assertIn("CPUs=128, current=128, LPI=256, perf=256", output.getvalue())
        self.assertIn("FD growth not checked", output.getvalue())


if __name__ == "__main__":
    unittest.main()
