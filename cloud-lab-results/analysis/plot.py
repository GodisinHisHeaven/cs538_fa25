#!/usr/bin/env python3
"""
Plot TCP RTT latency distribution from pcap files.
Usage: python3 plot_rtt_distribution.py <pcap_file> [pcap_file2 ...]
"""

import sys
import subprocess
import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path
from typing import List, Tuple

def extract_rtt_with_tshark(pcap_file: Path) -> Tuple[List[float], List[float]]:
    """Extract TCP RTT values and timestamps using tshark."""
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
        timestamps = []
        
        for line in result.stdout.strip().split('\n'):
            if line:
                parts = line.split(',')
                if len(parts) >= 1 and parts[0]:
                    # RTT is in seconds, convert to milliseconds
                    rtt_ms = float(parts[0]) * 1000
                    rtts.append(rtt_ms)
                    
                    # Timestamp in seconds
                    if len(parts) >= 2 and parts[1]:
                        timestamps.append(float(parts[1]))
                    else:
                        timestamps.append(0)
        
        return rtts, timestamps
    except subprocess.CalledProcessError as e:
        print(f"Error running tshark on {pcap_file}:", file=sys.stderr)
        print(f"  stderr: {e.stderr}", file=sys.stderr)
        return [], []
    except FileNotFoundError:
        print("Error: tshark not found. Please install Wireshark/tshark.", file=sys.stderr)
        sys.exit(1)

def plot_rtt_distribution(data_sets: List[Tuple[str, List[float], List[float]]]):
    """Plot RTT distribution as histogram and time series."""
    
    # Create figure with subplots
    fig, axes = plt.subplots(2, 1, figsize=(12, 10))
    
    # Color palette for multiple files
    colors = plt.cm.Set2(np.linspace(0, 1, len(data_sets)))
    
    # Plot 1: Histogram
    ax1 = axes[0]
    for idx, (label, rtts, _) in enumerate(data_sets):
        ax1.hist(rtts, bins=50, alpha=0.6, label=label, color=colors[idx], edgecolor='black')
    
    ax1.set_xlabel('RTT (ms)', fontsize=12)
    ax1.set_ylabel('Frequency', fontsize=12)
    ax1.set_title('TCP RTT Distribution', fontsize=14, fontweight='bold')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    # Add statistics annotation
    stats_text = []
    for label, rtts, _ in data_sets:
        if rtts:
            stats_text.append(
                f"{label}:\n"
                f"  Mean: {np.mean(rtts):.2f} ms\n"
                f"  Median: {np.median(rtts):.2f} ms\n"
                f"  p99: {np.percentile(rtts, 99):.2f} ms\n"
                f"  Samples: {len(rtts)}"
            )
    
    if stats_text:
        ax1.text(0.98, 0.97, '\n\n'.join(stats_text),
                transform=ax1.transAxes,
                fontsize=9,
                verticalalignment='top',
                horizontalalignment='right',
                bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.5))
    
    # Plot 2: Time series
    ax2 = axes[1]
    for idx, (label, rtts, timestamps) in enumerate(data_sets):
        if timestamps:
            ax2.scatter(timestamps, rtts, alpha=0.5, s=10, label=label, color=colors[idx])
        else:
            # If no timestamps, use index
            ax2.scatter(range(len(rtts)), rtts, alpha=0.5, s=10, label=label, color=colors[idx])
    
    ax2.set_xlabel('Time (seconds)' if data_sets[0][2] else 'Sample Index', fontsize=12)
    ax2.set_ylabel('RTT (ms)', fontsize=12)
    ax2.set_title('TCP RTT Over Time', fontsize=14, fontweight='bold')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    plt.tight_layout()
    
    # Save figure
    output_file = 'rtt_distribution.png'
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"\nPlot saved to: {output_file}")
    
    # Show plot
    plt.show()

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 plot_rtt_distribution.py <pcap_file> [pcap_file2 ...]")
        sys.exit(1)
    
    data_sets = []
    
    for pcap_path in sys.argv[1:]:
        pcap_file = Path(pcap_path)
        
        if not pcap_file.exists():
            print(f"Error: {pcap_file} not found", file=sys.stderr)
            continue
        
        print(f"Extracting RTT data from {pcap_file}...")
        rtts, timestamps = extract_rtt_with_tshark(pcap_file)
        
        if not rtts:
            print(f"  No RTT measurements found in {pcap_file}")
            continue
        
        print(f"  Found {len(rtts)} RTT measurements")
        print(f"  Min: {np.min(rtts):.3f} ms")
        print(f"  Mean: {np.mean(rtts):.3f} ms")
        print(f"  Median: {np.median(rtts):.3f} ms")
        print(f"  p99: {np.percentile(rtts, 99):.3f} ms")
        print(f"  Max: {np.max(rtts):.3f} ms")
        
        data_sets.append((pcap_file.name, rtts, timestamps))
    
    if not data_sets:
        print("No data to plot!")
        sys.exit(1)
    
    print("\nGenerating plots...")
    plot_rtt_distribution(data_sets)

if __name__ == '__main__':
    main()