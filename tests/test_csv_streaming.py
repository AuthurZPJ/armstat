#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Test CSV streaming performance and memory usage."""

import sys
import os
import json
import tempfile
import tracemalloc
from pathlib import Path

# Add scripts directory to path
ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

import plot_sum
import plot_cpu


def secure_temp_path(suffix: str) -> Path:
    """Create a private temporary file path without a name-generation race."""
    fd, name = tempfile.mkstemp(suffix=suffix)
    os.close(fd)
    return Path(name)


def generate_summary_csv(num_samples: int) -> Path:
    """Generate a test summary CSV file."""
    path = secure_temp_path(".csv")
    with path.open("w") as f:
        f.write("schema_version,interval,timestamp,timestamp_iso,Scope,freq,uncore_freq,busy_percent,idle_percent,temp0,temp1\n")
        for i in range(1, num_samples + 1):
            f.write(f"9,{i},1774665600,2026-03-28T10:40:00+0800,SUM,2200.00,1600.00,50.00,50.00,45.00,46.00\n")
    return path


def generate_cpu_csv(num_samples: int, num_cpus: int) -> Path:
    """Generate a test CPU CSV file."""
    path = secure_temp_path(".csv")
    with path.open("w") as f:
        f.write("schema_version,interval,timestamp,timestamp_iso,CPU,freq,busy_percent,idle_percent,temp\n")
        for i in range(1, num_samples + 1):
            for cpu in range(num_cpus):
                f.write(f"9,{i},1774665600,2026-03-28T10:40:00+0800,{cpu},2200.00,50.00,50.00,45.00\n")
    return path


def measure_memory(func, *args, **kwargs):
    """Measure peak memory usage of a function."""
    tracemalloc.start()
    result = func(*args, **kwargs)
    current, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    return result, peak


def test_summary_csv_streaming():
    """Test summary CSV streaming."""
    print("\n=== Summary CSV Streaming Test ===")
    
    # Generate test file with 10000 samples
    csv_path = generate_summary_csv(10000)
    
    try:
        # Test 1: Full load (no streaming)
        series1, mem1 = measure_memory(plot_sum.load_summary_series, csv_path)
        print(f"Full load: {len(series1.rows)} rows, {mem1 / 1024:.1f} KB peak memory")
        
        # Test 2: Streaming load with sample_range
        series2, mem2 = measure_memory(
            plot_sum.load_summary_series, csv_path, "100:200"
        )
        print(f"Stream load (100:200): {len(series2.rows)} rows, {mem2 / 1024:.1f} KB peak memory")
        
        # Verify correctness
        assert len(series1.rows) == 10000, f"Expected 10000 rows, got {len(series1.rows)}"
        assert len(series2.rows) == 101, f"Expected 101 rows (100-200 inclusive), got {len(series2.rows)}"
        
        # Verify memory reduction
        reduction = (mem1 - mem2) / mem1 * 100
        print(f"Memory reduction: {reduction:.1f}%")
        
        assert reduction > 50, "Summary range must reduce peak retained memory"
        print("✓ Streaming significantly reduces memory usage")
            
    finally:
        csv_path.unlink()


def test_cpu_csv_streaming():
    """Test CPU CSV streaming."""
    print("\n=== CPU CSV Streaming Test ===")
    
    # Generate test file with 1000 samples, 8 CPUs
    csv_path = generate_cpu_csv(1000, 8)
    
    try:
        # Test 1: Full load (no streaming)
        series1, mem1 = measure_memory(plot_cpu.load_cpu_series, csv_path)
        print(f"Full load: {len(series1.samples)} samples, {mem1 / 1024:.1f} KB peak memory")
        
        # Test 2: Streaming load with sample_range
        series2, mem2 = measure_memory(
            plot_cpu.load_cpu_series, csv_path, "100:200"
        )
        print(f"Stream load (100:200): {len(series2.samples)} samples, {mem2 / 1024:.1f} KB peak memory")
        
        # Verify correctness
        assert len(series1.samples) == 1000, f"Expected 1000 samples, got {len(series1.samples)}"
        assert len(series2.samples) == 101, f"Expected 101 samples (100-200 inclusive), got {len(series2.samples)}"
        
        # Verify memory reduction
        reduction = (mem1 - mem2) / mem1 * 100
        print(f"Memory reduction: {reduction:.1f}%")
        
        assert reduction > 50, "CPU range must reduce peak retained memory"
        print("✓ Streaming significantly reduces memory usage")

        projected, projected_memory = measure_memory(
            plot_cpu.load_cpu_series, csv_path, cpu_filter=[3], fields=["freq"]
        )
        assert len(projected.samples) == 1000
        assert projected.cpu_ids == [3]
        assert projected.numeric_fields == ["freq"]
        assert all(set(sample) == {3} and set(sample[3]) == {"freq"}
                   for sample in projected.samples)
        assert projected_memory < mem1 * 0.4, "CPU/field projection must occur before retention"
        print(f"CPU3/freq projection: {projected_memory / 1024:.1f} KB peak memory")
            
    finally:
        csv_path.unlink()


def test_json_range_streaming():
    """JSON now applies the same inclusive range while loading as CSV."""
    print("\n=== JSON Loading Test ===")
    
    # Generate a small JSON file
    json_path = secure_temp_path(".json")
    data = [
        {
            "schema_version": 9,
            "interval": i,
            "timestamp": 1774665600 + i,
            "summary": {"freq": 2200.0, "busy_percent": 50.0}
        }
        for i in range(1, 101)
    ]
    json_path.write_text(json.dumps(data))
    
    try:
        series1, _ = measure_memory(plot_sum.load_summary_series, json_path)
        series2, _ = measure_memory(plot_sum.load_summary_series, json_path, "10:20")
        
        assert len(series1.rows) == 100, f"Expected 100 rows, got {len(series1.rows)}"
        assert len(series2.rows) == 11, f"Expected 11 selected rows, got {len(series2.rows)}"
        assert [row["__interval_index"] for row in series2.rows] == list(range(10, 21))
        print("✓ JSON retains only the selected inclusive range")
        
    finally:
        json_path.unlink()


def test_json_cpu_projection_memory():
    """Memory follows selected samples/CPUs/fields, plus one decoded sample."""
    path = secure_temp_path(".json")
    try:
        with path.open("w", encoding="utf-8") as handle:
            handle.write("[")
            for index in range(1, 1001):
                if index > 1:
                    handle.write(",")
                json.dump({
                    "schema_version": 9, "interval": index,
                    "cpus": [
                        {"cpu": cpu, "freq": 2000.0 + index, "temp": 40.0 + cpu,
                         "busy_percent": 50.0, "idle_percent": 50.0,
                         "pmu": {f"event_{event}": index + event for event in range(8)}}
                        for cpu in range(16)
                    ],
                }, handle)
            handle.write("]")
        full, full_memory = measure_memory(plot_cpu.load_cpu_series, path)
        projected, projected_memory = measure_memory(
            plot_cpu.load_cpu_series, path, cpu_filter=[3], fields=["freq"]
        )
        window, window_memory = measure_memory(
            plot_cpu.load_cpu_series, path, "100:200", cpu_filter=[3], fields=["freq"]
        )
        assert len(full.samples) == len(projected.samples) == 1000
        assert len(window.samples) == 101
        assert projected.cpu_ids == window.cpu_ids == [3]
        assert window.numeric_fields == ["freq"]
        assert all(set(sample) == {3} and set(sample[3]) == {"freq"}
                   for sample in projected.samples)
        assert projected_memory < full_memory * 0.35
        assert window_memory < full_memory * 0.2
        print(f"JSON full: {full_memory / 1024:.1f} KB; CPU3/freq: "
              f"{projected_memory / 1024:.1f} KB; window: {window_memory / 1024:.1f} KB")
    finally:
        path.unlink()


if __name__ == "__main__":
    print("Testing CSV streaming optimization...")
    
    try:
        test_summary_csv_streaming()
        test_cpu_csv_streaming()
        test_json_range_streaming()
        test_json_cpu_projection_memory()
        
        print("\n" + "="*50)
        print("All streaming tests passed!")
        print("="*50)
        
    except AssertionError as e:
        print(f"\n✗ Test failed: {e}")
        sys.exit(1)
    except Exception as e:
        print(f"\n✗ Unexpected error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
