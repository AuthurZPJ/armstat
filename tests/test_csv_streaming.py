#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Test CSV streaming performance and memory usage."""

import sys
import os
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
            f.write(f"8,{i},1774665600,2026-03-28T10:40:00+0800,SUM,2200.00,1600.00,50.00,50.00,45.00,46.00\n")
    return path


def generate_cpu_csv(num_samples: int, num_cpus: int) -> Path:
    """Generate a test CPU CSV file."""
    path = secure_temp_path(".csv")
    with path.open("w") as f:
        f.write("schema_version,interval,timestamp,timestamp_iso,CPU,freq,busy_percent,idle_percent,temp\n")
        for i in range(1, num_samples + 1):
            for cpu in range(num_cpus):
                f.write(f"8,{i},1774665600,2026-03-28T10:40:00+0800,{cpu},2200.00,50.00,50.00,45.00\n")
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
        
        if reduction > 50:
            print("✓ Streaming significantly reduces memory usage")
        else:
            print("⚠ Streaming memory reduction is less than expected")
            
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
        
        if reduction > 50:
            print("✓ Streaming significantly reduces memory usage")
        else:
            print("⚠ Streaming memory reduction is less than expected")
            
    finally:
        csv_path.unlink()


def test_json_unchanged():
    """Verify JSON loading behavior is unchanged."""
    print("\n=== JSON Loading Test ===")
    
    # Generate a small JSON file
    json_path = secure_temp_path(".json")
    import json
    data = [
        {
            "schema_version": 8,
            "interval": i,
            "timestamp": 1774665600 + i,
            "summary": {"freq": 2200.0, "busy_percent": 50.0}
        }
        for i in range(1, 101)
    ]
    json_path.write_text(json.dumps(data))
    
    try:
        # Test: JSON should load fully regardless of sample_range
        series1, _ = measure_memory(plot_sum.load_summary_series, json_path)
        series2, _ = measure_memory(plot_sum.load_summary_series, json_path, "10:20")
        
        # Both should load all 100 samples (sample_range is ignored for JSON in load_summary_series)
        assert len(series1.rows) == 100, f"Expected 100 rows, got {len(series1.rows)}"
        assert len(series2.rows) == 100, f"Expected 100 rows (JSON loads fully), got {len(series2.rows)}"
        
        print(f"JSON loads fully: {len(series1.rows)} rows")
        print("✓ JSON behavior unchanged (loads fully, sample_range applied later)")
        
    finally:
        json_path.unlink()


if __name__ == "__main__":
    print("Testing CSV streaming optimization...")
    
    try:
        test_summary_csv_streaming()
        test_cpu_csv_streaming()
        test_json_unchanged()
        
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
