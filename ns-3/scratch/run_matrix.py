#!/usr/bin/env python3
"""
CS538 Experiment Matrix Orchestrator

Runs the baseline experiment matrix for the host-delay harness.
Generates a manifest.csv with all run results.

Baseline matrix:
- Workloads: pingpong, rpc
- Outstanding: 1, 8, 32
- Req/Rsp sizes: 256B, 1KB, 4KB
- Fixed network: 10Gbps, 50us, MTU 1500, qdisc=none

Total: 2 (workloads) × 3 (outstanding) × 3 (sizes) = 18 runs
"""

import os
import subprocess
import sys
import csv
import json
import time

# Configuration
NS3_ROOT = os.path.dirname(os.path.abspath(__file__))
NS3_BIN = os.path.join(NS3_ROOT, "..", "ns3")
OUT_DIR = "out/sim"

# Fixed network parameters
LINK_RATE = "10Gbps"
LINK_DELAY = "50us"
MTU = 1500
QDISC = "none"

# Experiment matrix
WORKLOADS = ["pingpong", "rpc"]
OUTSTANDING = [1, 8, 32]
SIZES = [
    (256, 256),    # 256B req/rsp
    (1024, 1024),  # 1KB req/rsp
    (4096, 4096),  # 4KB req/rsp
]

# Number of requests per run
N_REQ = 10000

def run_experiment(workload, outstanding, req_bytes, rsp_bytes, run_num, total_runs):
    """Run a single experiment"""
    print(f"\n{'='*70}")
    print(f"Run {run_num}/{total_runs}: {workload}, out={outstanding}, req/rsp={req_bytes}B")
    print(f"{'='*70}")

    # Build command - note: ns3 script requires "--" separator between ns3 args and program args
    cmd = [
        NS3_BIN,
        "run",
        "hd_runner",
        "--",
        f"--linkRate={LINK_RATE}",
        f"--linkDelay={LINK_DELAY}",
        f"--mtu={MTU}",
        f"--qdisc={QDISC}",
        f"--workload={workload}",
        f"--nReq={N_REQ}",
        f"--outstanding={outstanding}",
        f"--reqBytes={req_bytes}",
        f"--rspBytes={rsp_bytes}",
        "--enableEgressHook=1",
        "--enableIngressHook=1",
        "--runId=auto",
        f"--outDir={OUT_DIR}",
    ]

    print(f"Command: {' '.join(cmd)}")

    # Run experiment
    start_time = time.time()
    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True, cwd=NS3_ROOT + "/..")
        elapsed = time.time() - start_time

        # Extract run ID from output
        run_id = None
        out_path = None
        for line in result.stdout.split('\n'):
            if 'Run ID:' in line:
                run_id = line.split('Run ID:')[1].strip()
            elif 'Results written to:' in line:
                out_path = line.split('Results written to:')[1].strip()

        if not run_id or not out_path:
            print("Warning: Could not extract run ID from output")
            print(result.stdout)
            return None

        print(f"✓ Run completed in {elapsed:.1f}s")
        print(f"  Run ID: {run_id}")
        print(f"  Output: {out_path}")

        # Read summary stats
        config_path = os.path.join(out_path, "config.json")
        summary_path = os.path.join(out_path, "summary.txt")

        p50, p95, p99, completed = extract_stats(summary_path)

        return {
            "run_id": run_id,
            "workload": workload,
            "outstanding": outstanding,
            "req_bytes": req_bytes,
            "rsp_bytes": rsp_bytes,
            "linkRate": LINK_RATE,
            "linkDelay": LINK_DELAY,
            "mtu": MTU,
            "qdisc": QDISC,
            "p50_ns": p50,
            "p95_ns": p95,
            "p99_ns": p99,
            "completed": completed,
            "out_dir": out_path,
            "elapsed_s": f"{elapsed:.1f}",
        }

    except subprocess.CalledProcessError as e:
        print(f"✗ Run failed!")
        print(f"  Error: {e}")
        print(f"  Stdout: {e.stdout}")
        print(f"  Stderr: {e.stderr}")
        return None

def extract_stats(summary_path):
    """Extract p50/p95/p99 and completed count from summary.txt"""
    p50 = p95 = p99 = completed = 0

    try:
        with open(summary_path, 'r') as f:
            content = f.read()

            # Parse completed count
            for line in content.split('\n'):
                if 'Completed:' in line:
                    completed = int(line.split(':')[1].strip().split('/')[0])

            # Parse latency stats (in ns)
            in_ns_section = False
            for line in content.split('\n'):
                if 'Latency (ns):' in line:
                    in_ns_section = True
                elif 'Latency (μs):' in line:
                    in_ns_section = False
                elif in_ns_section:
                    if 'p50:' in line:
                        p50 = int(float(line.split(':')[1].strip()))
                    elif 'p95:' in line:
                        p95 = int(float(line.split(':')[1].strip()))
                    elif 'p99:' in line:
                        p99 = int(float(line.split(':')[1].strip()))

    except Exception as e:
        print(f"Warning: Could not parse summary file: {e}")

    return p50, p95, p99, completed

def write_manifest(results):
    """Write manifest.csv with all run results"""
    manifest_path = os.path.join(OUT_DIR, "manifest.csv")

    fieldnames = [
        "run_id",
        "workload",
        "outstanding",
        "req_bytes",
        "rsp_bytes",
        "linkRate",
        "linkDelay",
        "mtu",
        "qdisc",
        "p50_ns",
        "p95_ns",
        "p99_ns",
        "completed",
        "elapsed_s",
        "out_dir",
    ]

    with open(manifest_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(results)

    print(f"\n✓ Manifest written to: {manifest_path}")

def main():
    print("CS538 Experiment Matrix Orchestrator")
    print("=" * 70)

    # Calculate total runs
    total_runs = len(WORKLOADS) * len(OUTSTANDING) * len(SIZES)
    print(f"Total runs: {total_runs}")
    print(f"  Workloads: {WORKLOADS}")
    print(f"  Outstanding: {OUTSTANDING}")
    print(f"  Sizes: {[f'{s[0]}B' for s in SIZES]}")
    print()

    # Ensure output directory exists
    os.makedirs(OUT_DIR, exist_ok=True)

    # Run all experiments
    results = []
    run_num = 0

    for workload in WORKLOADS:
        for outstanding in OUTSTANDING:
            for req_bytes, rsp_bytes in SIZES:
                run_num += 1
                result = run_experiment(workload, outstanding, req_bytes, rsp_bytes, run_num, total_runs)

                if result:
                    results.append(result)
                else:
                    print(f"Warning: Run {run_num} failed, skipping...")

    # Write manifest
    if results:
        write_manifest(results)

        print("\n" + "=" * 70)
        print("Experiment matrix complete!")
        print(f"Successful runs: {len(results)}/{total_runs}")
        print(f"Output directory: {OUT_DIR}")
        print("=" * 70)

        # Print summary table
        print("\nResults Summary:")
        print("-" * 70)
        print(f"{'Workload':<10} {'Out':<5} {'Size':<8} {'p50(μs)':<10} {'p95(μs)':<10} {'p99(μs)':<10}")
        print("-" * 70)
        for r in results:
            size_str = f"{r['req_bytes']}B"
            print(f"{r['workload']:<10} {r['outstanding']:<5} {size_str:<8} "
                  f"{r['p50_ns']/1000:<10.2f} {r['p95_ns']/1000:<10.2f} {r['p99_ns']/1000:<10.2f}")
        print("-" * 70)

    else:
        print("\n✗ No successful runs!")
        sys.exit(1)

if __name__ == "__main__":
    main()
