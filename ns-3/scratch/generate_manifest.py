#!/usr/bin/env python3
"""
Generate manifest.csv from completed runs
"""

import os
import csv
import json

OUT_DIR = "../out/sim"

def parse_config(config_path):
    """Parse config.json file"""
    with open(config_path, 'r') as f:
        return json.load(f)

def parse_summary(summary_path):
    """Parse summary.txt file"""
    p50 = p95 = p99 = completed = 0

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

    return p50, p95, p99, completed

def main():
    results = []

    # Find all run directories
    for run_dir in sorted(os.listdir(OUT_DIR)):
        run_path = os.path.join(OUT_DIR, run_dir)

        if not os.path.isdir(run_path):
            continue

        config_path = os.path.join(run_path, "config.json")
        summary_path = os.path.join(run_path, "summary.txt")

        if not os.path.exists(config_path) or not os.path.exists(summary_path):
            print(f"Skipping {run_dir}: missing files")
            continue

        try:
            # Parse config
            config = parse_config(config_path)

            # Parse summary
            p50, p95, p99, completed = parse_summary(summary_path)

            # Add to results
            results.append({
                "run_id": config["runId"],
                "workload": config["workload"],
                "outstanding": config["outstanding"],
                "req_bytes": config["reqBytes"],
                "rsp_bytes": config["rspBytes"],
                "linkRate": config["linkRate"],
                "linkDelay": config["linkDelay"],
                "mtu": config["mtu"],
                "qdisc": config["qdisc"],
                "p50_ns": p50,
                "p95_ns": p95,
                "p99_ns": p99,
                "completed": completed,
                "out_dir": run_path,
            })

            print(f"✓ Processed {run_dir}")

        except Exception as e:
            print(f"Error processing {run_dir}: {e}")

    # Write manifest
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
        "out_dir",
    ]

    with open(manifest_path, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(results)

    print(f"\n✓ Manifest written to: {manifest_path}")
    print(f"  Total runs: {len(results)}")

    # Print summary table
    print("\nResults Summary:")
    print("-" * 80)
    print(f"{'Workload':<10} {'Out':<5} {'Size':<8} {'p50(μs)':<10} {'p95(μs)':<10} {'p99(μs)':<10}")
    print("-" * 80)
    for r in results:
        size_str = f"{r['req_bytes']}B"
        print(f"{r['workload']:<10} {r['outstanding']:<5} {size_str:<8} "
              f"{r['p50_ns']/1000:<10.2f} {r['p95_ns']/1000:<10.2f} {r['p99_ns']/1000:<10.2f}")
    print("-" * 80)

if __name__ == "__main__":
    main()
