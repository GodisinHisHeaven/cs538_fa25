# CS538 Host Delay Experiment Harness

A deterministic ns-3 experiment harness for measuring host-delay effects on network tail latency.

## Overview

This harness implements a model-free baseline for studying host-induced delays in datacenter networks. It provides:

- **Deterministic topology**: Host0 → Host1 via PointToPoint link
- **Multiple workloads**: ping-pong and RPC patterns
- **No-op delay hooks**: `DelayEgress` and `DelayIngress` for future model integration
- **Comprehensive logging**: Per-request latencies (JSONL) and optional event timelines
- **Summary statistics**: p50, p95, p99 latencies

## Files

- `hd_runner.cc` - Main simulation runner
- `delay_hooks.h` - Hook interface definitions
- `delay_hooks.cc` - No-op hook implementations
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

## Hook Integration (Future)

The harness is ready for model integration:

### Hook Contract

1. **`DelayEgress(nodeId, bytes, seq)`** - Called before NIC Tx
2. **`DelayIngress(nodeId, bytes, seq)`** - Called before app delivery

### Integration Steps

1. Implement delay logic in `delay_hooks.cc`
2. Load model config via `--hookConfigPath`
3. Return non-zero `Time` values from hooks
4. Verify effects in `events.jsonl` (pre/post-hook timestamps)

No changes to `hd_runner.cc` required!

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
