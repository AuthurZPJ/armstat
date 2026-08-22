#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Demonstrate CSV streaming with real armstat output."""

import subprocess
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def create_large_test_data():
    """Create test data simulating long-running monitoring."""
    print("Creating test data (1000 samples)...")
    
    # Create a temporary directory for test files
    tmpdir = Path(tempfile.mkdtemp())
    
    # Generate summary CSV using armstat
    summary_csv = tmpdir / "summary.csv"
    cmd = [
        str(ROOT / "armstat"),
        "-S",  # Summary only
        "-f", "csv",
        "-O", str(summary_csv),
        "-i", "0.01",  # 10ms interval; 1000 samples complete in about 10s
        "-n", "1000"  # 1000 samples
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            print(f"⚠ Could not run armstat: {result.stderr}")
            print("  This is expected when armstat is not built or the host lacks Linux telemetry")
            shutil.rmtree(tmpdir, ignore_errors=True)
            return None
    except (subprocess.TimeoutExpired, FileNotFoundError) as e:
        print(f"⚠ Could not run armstat: {e}")
        shutil.rmtree(tmpdir, ignore_errors=True)
        return None
    
    return tmpdir, summary_csv


def demo_streaming():
    """Demonstrate streaming vs full load."""
    result = create_large_test_data()
    if result is None:
        print("\nSkipping demo (armstat not available)")
        return
    
    tmpdir, csv_path = result
    
    try:
        import sys
        import tracemalloc
        
        sys.path.insert(0, str(ROOT / "scripts"))
        import plot_sum
        
        # Get file size
        file_size = csv_path.stat().st_size
        print(f"\nTest file: {csv_path}")
        print(f"File size: {file_size / 1024:.1f} KB")
        
        # Test 1: Load all samples
        print("\n1. Loading all samples...")
        tracemalloc.start()
        series_all = plot_sum.load_summary_series(csv_path)
        _, peak_all = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        print(f"   Loaded: {len(series_all.rows)} samples")
        print(f"   Peak memory: {peak_all / 1024:.1f} KB")
        
        # Test 2: Stream samples 100-200
        print("\n2. Streaming samples 100-200...")
        tracemalloc.start()
        series_stream = plot_sum.load_summary_series(csv_path, sample_range="100:200")
        _, peak_stream = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        print(f"   Loaded: {len(series_stream.rows)} samples")
        print(f"   Peak memory: {peak_stream / 1024:.1f} KB")
        
        # Test 3: Stream last 50 samples
        print("\n3. Streaming last 50 samples (951:1000)...")
        tracemalloc.start()
        series_tail = plot_sum.load_summary_series(csv_path, sample_range="951:")
        _, peak_tail = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        print(f"   Loaded: {len(series_tail.rows)} samples")
        print(f"   Peak memory: {peak_tail / 1024:.1f} KB")
        
        # Summary
        print("\n" + "="*60)
        print("Memory usage comparison:")
        print(f"  All samples:      {peak_all / 1024:8.1f} KB")
        print(f"  Samples 100-200:  {peak_stream / 1024:8.1f} KB  ({(1 - peak_stream/peak_all)*100:.1f}% reduction)")
        print(f"  Samples 951-1000: {peak_tail / 1024:8.1f} KB  ({(1 - peak_tail/peak_all)*100:.1f}% reduction)")
        print("="*60)
        
        # Show example usage
        print("\nExample command-line usage:")
        print(f"  # Load all samples (uses {peak_all / 1024:.0f} KB)")
        print(f"  python3 scripts/plot_sum.py {csv_path} --preset power")
        print()
        print(f"  # Stream only samples 100-200 (uses {peak_stream / 1024:.0f} KB)")
        print(f"  python3 scripts/plot_sum.py {csv_path} --preset power --sample-range 100:200")
        
    finally:
        # Cleanup
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    print("CSV Streaming Demo")
    print("="*60)
    demo_streaming()
