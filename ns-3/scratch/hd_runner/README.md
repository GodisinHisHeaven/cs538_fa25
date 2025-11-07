# CS538 Host Delay Experiment Harness

A deterministic ns-3 experiment harness for measuring host-delay effects on network tail latency.

## Overview

This harness implements a cache miss + queueing delay model for studying host-induced delays in datacenter networks. It provides:

- **Deterministic topology**: Host0 → Host1 via PointToPoint link
- **Multiple workloads**: ping-pong and RPC patterns
- **Host delay model**: Cache miss penalty + queueing delay with load-dependent step functions
- **Configurable severity**: Three profiles (realistic, moderate, severe) for different experiment scenarios
- **Comprehensive logging**: Per-request latencies (JSONL) and optional event timelines
- **Summary statistics**: p50, p95, p99 latencies

## Files

- `hd_runner.cc` - Main simulation runner
- `delay_hooks.h` - Hook interface definitions
- `delay_hooks.cc` - **Cache miss + queueing delay model implementation**
- `../run_matrix.py` - Orchestration script for running experiment matrix
- `../generate_manifest.py` - Post-processing script to create manifest.csv

## Building

```bash
cd /path/to/ns-3
./ns3 configure
./ns3 build hd_runner
```

## Running

### Single Run

```bash
./ns3 run hd_runner -- --nReq=10000 --workload=rpc --outstanding=8 --reqBytes=1024 --rspBytes=1024
```

### Full Experiment Matrix

```bash
cd scratch
python3 run_matrix.py
```

This runs 18 baseline experiments:
- Workloads: pingpong, rpc
- Outstanding: 1, 8, 32
- Sizes: 256B, 1KB, 4KB

### Generate Manifest

After runs complete:

```bash
cd scratch
python3 generate_manifest.py
```

## Command-Line Options

| Option | Default | Description |
|--------|---------|-------------|
| `--linkRate` | 10Gbps | Link data rate |
| `--linkDelay` | 50us | Link propagation delay |
| `--mtu` | 1500 | MTU size |
| `--qdisc` | none | Queue discipline (none\|fq_codel) |
| `--workload` | pingpong | Workload type (pingpong\|rpc) |
| `--nReq` | 10000 | Number of requests |
| `--outstanding` | 1 | Outstanding requests |
| `--reqBytes` | 1024 | Request size in bytes |
| `--rspBytes` | 1024 | Response size in bytes |
| `--enableEgressHook` | 1 | Enable egress hook |
| `--enableIngressHook` | 1 | Enable ingress hook |
| `--hookConfigPath` | "" | Path to hook config (future) |
| `--seed` | 1 | Random seed |
| `--runId` | auto | Run ID |
| `--outDir` | out/sim | Output directory |

## Output Structure

Each run creates a directory: `out/sim/<run-id>/`

### Files Generated

- **`config.json`** - Complete run configuration
- **`rpc.jsonl`** - Per-request latency records
  ```json
  {"seq":42,"t_send_ns":1234567890,"t_recv_ns":1235567890,"lat_ns":1000000}
  ```
- **`events.jsonl`** - Event timeline (optional)
  ```json
  {"t_ns":1234567890,"node":0,"event":"tx_app","seq":42,"len":1024}
  ```
- **`summary.txt`** - Human-readable summary with p50/p95/p99

### Manifest File

After running the matrix, `out/sim/manifest.csv` contains:

```csv
run_id,workload,outstanding,req_bytes,rsp_bytes,linkRate,linkDelay,mtu,qdisc,p50_ns,p95_ns,p99_ns,completed,out_dir
```

## Baseline Results

From the 18-run baseline matrix (no host delay):

| Workload | Outstanding | Size | p50 (μs) | p95 (μs) | p99 (μs) |
|----------|-------------|------|----------|----------|----------|
| pingpong | 1 | 256B | 100.46 | 100.46 | 100.46 |
| pingpong | 1 | 1KB | 101.69 | 101.69 | 101.69 |
| pingpong | 1 | 4KB | 106.67 | 106.67 | 106.67 |
| rpc | 8 | 1KB | 101.69 | 101.69 | 101.69 |
| rpc | 32 | 4KB | 106.78 | 106.78 | 106.78 |

**Key observations:**
- With no-op hooks, median RTT ≈ 2×linkDelay (100μs) + small protocol overhead
- Latencies are deterministic (p50 = p95 = p99) as expected with no queueing or delay
- Larger packets show slightly higher latency due to serialization

## Host Delay Model

### Model Formula

```
Host_delay = base_CPU_cycles + (cache_misses × penalty_P(L)) + queueing_delay_Q(L)
```

Where:
- **base_CPU_cycles**: 150ns (base packet processing overhead)
- **cache_misses**: Probabilistic function of packet size
  - Small packets (≤256B): 10% miss probability
  - Medium packets (256B-1KB): 40% miss probability
  - Large packets (>1KB): 70% miss probability
- **penalty_P(L)**: Cache miss penalty with step function
  - Below load threshold (L<0.7): `100ns × severity`
  - Above load threshold (L≥0.7): `300ns × severity` (3x multiplier)
- **queueing_delay_Q(L)**: Queueing delay with step function and growth
  - Below load threshold (L<0.6): `50ns × severity`
  - Above load threshold (L≥0.6): `50ns + 100ns×(L-0.6) × severity`
- **L (load factor)**: Calculated from rolling 100ms window packet rate

### Configuration Profiles

Use `--hookConfigPath` to select delay model severity:

| Profile | Severity | Expected Impact | Use Case |
|---------|----------|-----------------|----------|
| `""` or `realistic` | 1.0 | ~1-2μs | Matches CloudLab measurements |
| `moderate` | 3.0 | ~5-15μs | Visible tail latency for experiments |
| `severe` | 10.0 | ~20-50μs | Extreme congestion scenarios |

**Examples:**
```bash
# Realistic (default) - matches real-world data
./ns3 run hd_runner -- --nReq=10000 --outstanding=8

# Moderate - visible tail latency
./ns3 run hd_runner -- --nReq=10000 --outstanding=32 --hookConfigPath=moderate

# Severe - extreme congestion
./ns3 run hd_runner -- --nReq=10000 --outstanding=32 --hookConfigPath=severe
```

### Model Behavior

The model exhibits realistic threshold behavior:

1. **Low Load (L < 0.6)**: Minimal impact from base CPU cycles and small cache penalties
2. **Medium Load (0.6 ≤ L < 0.7)**: Queueing delay starts growing
3. **High Load (L ≥ 0.7)**: Both queueing and cache miss penalties increase (step functions activate)

**Sample Results (1KB packets, moderate config):**

| Outstanding | Load Factor | Delay | p50 (μs) | p99 (μs) |
|-------------|-------------|-------|----------|----------|
| 1 | 1.87 | ~7μs | 106.49 | 106.49 |
| 32 | 2.0 | ~7μs | 115.15 | 125.03 |

The p99 tail latency becomes visible under high load, demonstrating the model's ability to capture queueing and cache contention effects.

### Hook Implementation

Both `DelayEgress()` and `DelayIngress()` implement the full model:

1. Calculate current load factor from rolling packet timestamps
2. Estimate cache misses from packet size
3. Apply load-dependent penalty (step function at L=0.7)
4. Apply load-dependent queueing delay (step function at L=0.6)
5. Return total delay = base + (misses × penalty) + queue

## Known Issues and Deviations

1. **Topology simplification**: Using direct P2P link instead of explicit switch node. This is functionally equivalent for our deterministic scenario.

2. **Workload naming**: Both "pingpong" and "rpc" currently use the same RPC implementation. Future work can differentiate them (e.g., make pingpong use UdpEcho* apps).

3. **QDisc support**: Currently only "none" is tested. FQ-CoDel support is plumbed but not validated.

4. **Event logging**: Always enabled. Could add `--enableEventLog` flag to disable for performance.

5. **Determinism**: All runs with same `--seed` produce identical results, as required.

## Acceptance Checks

✅ **Sanity**: Median RTT ≈ 2×linkDelay (100μs ≈ 2×50μs)
✅ **Load effect**: Outstanding parameter affects completion time (not tail latency in no-op case)
✅ **Reproducibility**: Same seed produces identical `summary.txt`

## Architecture

```
hd_runner.cc
├── RpcClientApp (custom application)
│   ├── SendRequest()
│   │   └── DelayEgress() hook
│   └── HandleResponse()
│       └── DelayIngress() hook
├── RpcServerApp (simple echo)
└── Logging infrastructure
    ├── LogRpcRecord()
    ├── LogEvent()
    └── Summary generation
```

## Contact

For questions or issues, refer to the CS538 course staff.

---

**Note**: This is a model-free baseline. Hook effects will be visible once the delay model is integrated.
