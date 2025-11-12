#!/usr/bin/env python3
"""
Analyze TCP RTT latency from pcap files captured during iperf3 tests.
Usage: python3 parse_pcap.py <directory_or_pcap_file> [file2 ...]
"""

import sys
import subprocess
import json
import numpy as np
from datetime import datetime
from pathlib import Path
from typing import Dict, List

def find_pcap_files(path: Path) -> List[Path]:
    """Find all pcap files in a directory or return the file if it's a pcap."""
    if path.is_file():
        if path.suffix.lower() in ['.pcap']:
            return [path]
        else:
            print(f"[WARNING] {path} is not a pcap file, skipping...")
            return []
    elif path.is_dir():
        # Find all pcap files recursively
        pcap_files = []
        for pattern in ['*.pcap']:
            pcap_files.extend(path.rglob(pattern))
        return pcap_files
    else:
        print(f"[WARNING] {path} not found, skipping...")
        return []

def extract_rtt_with_tshark(pcap_file) -> List[float]:
    """Extract TCP RTT values using tshark."""
    cmd = [
        'tshark', '-r', str(pcap_file),
        '-Y', 'tcp.analysis.ack_rtt',
        '-T', 'fields',
        '-e', 'tcp.analysis.ack_rtt',
        '-e', 'frame.time_relative',
        '-E', 'separator=,'
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        rtts = []
        
        for line in result.stdout.strip().split('\n'):
            if line:
                parts = line.split(',')
                if len(parts) >= 1 and parts[0]:
                    # RTT is in seconds, convert to milliseconds
                    rtt_ms = float(parts[0]) * 1000
                    rtts.append(rtt_ms)
        
        return rtts
    except subprocess.CalledProcessError as e:
        print(f"Error running tshark on {pcap_file}:", file=sys.stderr)
        print(f"  Return code: {e.returncode}", file=sys.stderr)
        print(f"  stderr: {e.stderr}", file=sys.stderr)
        print(f"  stdout: {e.stdout}", file=sys.stderr)
        return []
    except FileNotFoundError:
        print("Error: tshark not found. Please install Wireshark/tshark.", file=sys.stderr)
        sys.exit(1)

def calculate_statistics(values) -> Dict[str, any]:
    """Calculate statistics."""
    if not values:
        return {}
    
    return {
        'min': np.min(values),
        'p50': np.percentile(values, 50),
        'p90': np.percentile(values, 90),
        'p95': np.percentile(values, 95),
        'p99': np.percentile(values, 99),
        'p999': np.percentile(values, 99.9),
        'max': np.max(values),
        'mean': np.mean(values),
        'std': np.std(values),
        'count': len(values)
    }

def main():
    if len(sys.argv) < 2:
        print("Usage: python parse_pcap.py <directory_or_pcap_file> [file2 ...]")
        sys.exit(1)
    
    # Collect all pcap files from all provided paths
    all_pcap_files = []
    for arg in sys.argv[1:]:
        path = Path(arg)
        all_pcap_files.extend(find_pcap_files(path))
    
    if not all_pcap_files:
        print("No pcap files found!")
        sys.exit(1)
    
    print(f"Found {len(all_pcap_files)} pcap file(s)")
    print()
    
    print("=" * 80)
    print("TCP RTT Latency Analysis")
    print("=" * 80)
    print()
    
    all_results = []
    all_rtts = []
    
    for pcap_file in all_pcap_files:
        print(f"Analyzing: {pcap_file}")
        print("-" * 80)
        
        rtts = extract_rtt_with_tshark(pcap_file)
        
        if not rtts:
            print("  No RTT measurements found in this file.\n")
            continue
        
        stats = calculate_statistics(rtts)
        all_rtts.extend(rtts)

        result = {
            'file': str(pcap_file),
            'stats': stats
        }
        all_results.append(result)
        
        print(f"  Sample count: {stats['count']}")
        print(f"  Min RTT:      {stats['min']:.3f} ms")
        print(f"  Mean RTT:     {stats['mean']:.3f} ms")
        print(f"  Median (p50): {stats['p50']:.3f} ms")
        print(f"  p90:          {stats['p90']:.3f} ms")
        print(f"  p95:          {stats['p95']:.3f} ms")
        print(f"  p99:          {stats['p99']:.3f} ms")
        print(f"  p99.9:        {stats['p999']:.3f} ms")
        print(f"  Max RTT:      {stats['max']:.3f} ms")
        print(f"  Std Dev:      {stats['std']:.3f} ms")
        print()
    
    # Summary comparison if multiple files
    if len(all_results) > 1:
        print("=" * 80)
        print("SUMMARY COMPARISON (p99 latency)")
        print("=" * 80)
        
        print(f"{'File':<50} {'p99 (ms)':<20} {'Mean (ms)':<20} {'Samples':<10}")
        print("-" * 80)
        
        for result in all_results:
            file = Path(result['file']).name
            p99 = result['stats']['p99']
            mean = result['stats']['mean']
            count = result['stats']['count']
            
            print(f"{file:<50} {p99:<20.3f} {mean:<20.3f} {count:<10}")
        
        print()
        
        # Overall statistics across all files
        if all_rtts:
            overall_stats = calculate_statistics(all_rtts)
            print("=" * 80)
            print("OVERALL STATISTICS")
            print("=" * 80)
            print(f"  Total samples: {overall_stats['count']}")
            print(f"  Min RTT:      {overall_stats['min']:.3f} ms")
            print(f"  Mean RTT:     {overall_stats['mean']:.3f} ms")
            print(f"  Median (p50): {overall_stats['p50']:.3f} ms")
            print(f"  p90:          {overall_stats['p90']:.3f} ms")
            print(f"  p95:          {overall_stats['p95']:.3f} ms")
            print(f"  p99:          {overall_stats['p99']:.3f} ms")
            print(f"  p99.9:        {overall_stats['p999']:.3f} ms")
            print(f"  Max RTT:      {overall_stats['max']:.3f} ms")
            print(f"  Std Dev:      {overall_stats['std']:.3f} ms")
            print()
    
    # Save results to JSON
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    output_file = Path(f'rtt_analysis_{timestamp}.json')

    overall_stats = calculate_statistics(all_rtts) if all_rtts else {}

    with open(output_file, 'w') as f:
        json.dump({
            'individual_results': all_results,
            'combined_results': overall_stats
        }, f, indent=2)
    
    print(f"Results saved to: {output_file}")

if __name__ == '__main__':
    main()