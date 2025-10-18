# CS538 Host Delay Experiment - Architecture Diagram

## 1. Network Topology

```
┌─────────────────────────────────────────────────────────────────┐
│                    Deterministic Network Path                   │
│                                                                 │
│   ┌──────────┐      Point-to-Point Link        ┌──────────┐     │
│   │          │  ────────────────────────────►  │          │     │
│   │  Host 0  │                                 │  Host 1  │     │
│   │ (Client) │  ◄────────────────────────────  │ (Server) │     │
│   │          │                                 │          │     │
│   └──────────┘                                 └──────────┘     │
│                                                                 │
│   Link Rate:  10 Gbps                                           │
│   Link Delay: 50 μs (one-way)                                   │
│   MTU:        1500 bytes                                        │
│   Queue:      Simple FIFO (no qdisc)                            │
└─────────────────────────────────────────────────────────────────┘
```

## 2. Packet Flow with Delay Hooks

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              REQUEST PATH                               │
└─────────────────────────────────────────────────────────────────────────┘

Host 0 (Client)                                        Host 1 (Server)
┌──────────────┐                                       ┌──────────────┐
│              │                                       │              │
│  RPC Client  │                                       │  RPC Server  │
│     App      │                                       │     App      │
│              │                                       │              │
└──────┬───────┘                                       └──────▲───────┘
       │                                                      │
       │ 1. Send Request                                      │
       │    (t_send_ns)                                       │
       ▼                                                      │
┌──────────────┐                                              │
│ DelayEgress  │ ◄─── Hook called before NIC Tx               │
│   (NO-OP)    │      Returns: 0 delay                        │
└──────┬───────┘                                              │
       │                                                      │
       │ 2. Egress Hook Applied                               │
       │    (currently no delay)                              │
       ▼                                                      │
┌──────────────┐                                              │
│  NIC Tx (L2) │ -─────────────────────────────┐              │
└──────────────┘                               │              │
                                               │              │
                           3. Network Transit  │              │
                              (~100 μs RTT)    │              │
                                               │              │
                                               ▼              │
                                        ┌──────────────┐      │
                                        │  NIC Rx (L2) │      │
                                        └──────┬───────┘      │
                                               │              │
                                               │ 4. Receive   │
                                               ▼              │
                                        ┌──────────────┐      │
                                        │DelayIngress  │      │
                                        │   (NO-OP)    │ ◄── Hook before app
                                        └──────┬───────┘      │
                                               │              │
                                               │ 5. Ingress   │
                                               │    Applied   │
                                               └─────────────-┘

┌─────────────────────────────────────────────────────────────────────────┐
│                             RESPONSE PATH                               │
│                          (Same hooks apply)                             │
└─────────────────────────────────────────────────────────────────────────┘
```

## 3. Software Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│                         hd_runner (Main)                          │
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌─────────────────┐              ┌──────────────────┐            │
│  │  Configuration   │              │   Delay Hooks    │            │
│  │   - CLI Parser  │              │  - DelayEgress() │            │
│  │   - Run Config   │              │  - DelayIngress()│            │
│  │   - Seed Mgmt   │              │  - NO-OP (now)   │            │
│  └─────────────────┘              └──────────────────┘            │
│                                                                   │
│  ┌──────────────────────────────────────────────────────┐         │
│  │              Application Layer                       │         │
│  │  ┌──────────────┐         ┌───────────────┐          │         │
│  │  │ RpcClientApp │         │ RpcServerApp  │          │         │
│  │  │              │         │               │          │         │
│  │  │ - SendReq()  │         │ - HandleReq() │          │         │
│  │  │ - HandleRsp()│         │ - SendRsp()   │          │         │
│  │  │ - Track RTT  │         │               │          │         │
│  │  └──────────────┘         └───────────────┘          │         │
│  └──────────────────────────────────────────────────────┘         │
│                                                                   │
│  ┌──────────────────────────────────────────────────────┐         │
│  │           Instrumentation & Logging                  │         │
│  │  - LogRpcRecord()      → rpc.jsonl                   │         │
│  │  - LogEvent()          → events.jsonl                │         │
│  │  - WriteConfig()       → config.json                   │         │
│  │  - WriteSummary()      → summary.txt                 │         │
│  │  - Percentile Calc     → p50/p95/p99                 │         │
│  └──────────────────────────────────────────────────────┘         │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

## 4. Experiment Workflow

```
┌────────────────────────────────────────────────────────────────┐
│                    Experiment Orchestration                    │
└────────────────────────────────────────────────────────────────┘

  ┌──────────────────┐
  │  run_matrix.py   │  Iterates through experiment matrix
  │                  │  - 2 workloads (pingpong, rpc)
  │  Orchestrator    │  - 3 outstanding (1, 8, 32)
  │                  │  - 3 sizes (256B, 1KB, 4KB)
  └────────┬─────────┘  = 18 runs total
           │
           │ For each configuration:
           │
           ▼
  ┌──────────────────┐
  │   ./ns3 run      │  Launches hd_runner with params
  │   hd_runner      │  --workload=rpc --outstanding=8
  │                  │  --reqBytes=1024 --rspBytes=1024
  └────────┬─────────┘
           │
           │ Runs simulation
           │
           ▼
  ┌──────────────────┐
  │  Simulation Run  │  - 10,000 requests
  │                  │  - Deterministic (seed-based)
  │  ┌────────────┐  │  - Latency tracking
  │  │  Client    │  │  - Event logging
  │  │     ↕      │  │
  │  │  Server    │  │
  │  └────────────┘  │
  └────────┬─────────┘
           │
           │ Generates output
           │
           ▼
  ┌──────────────────────────────┐
  │  out/sim/<run-id>/           │
  │  ├── config.json              │
  │  ├── rpc.jsonl               │
  │  ├── events.jsonl            │
  │  └── summary.txt             │
  └──────────────────────────────┘
           │
           │ After all runs:
           │
           ▼
  ┌──────────────────┐
  │ generate_        │  Aggregates all results
  │ manifest.py      │  → manifest.csv
  │                  │  → EXPERIMENT_SUMMARY.txt
  └──────────────────┘
```

## 5. Data Flow & Logging

```
┌───────────────────────────────────────────────────────────────┐
│                      Per-Request Tracking                     │
└───────────────────────────────────────────────────────────────┘

Request Lifecycle:
─────────────────

1. Client sends request (seq=42)
   ├─► t_send_ns = 1000000000
   ├─► LogEvent(tx_app, seq=42, len=1024)
   └─► DelayEgress(bytes=1024, seq=42) → 0 delay
       └─► LogEvent(tx_post_egress, seq=42)

2. Network transit (~100 μs)

3. Server receives
   ├─► DelayIngress(bytes=1024, seq=42) → 0 delay
   ├─► LogEvent(rx_post_ingress, seq=42)
   └─► Send response immediately

4. Client receives response (seq=42)
   ├─► t_recv_ns = 1000101686
   ├─► lat_ns = t_recv_ns - t_send_ns = 101686 ns
   └─► LogRpcRecord(seq=42, t_send, t_recv, lat)

Output Files:
─────────────

rpc.jsonl:
{"seq":42,"t_send_ns":1000000000,"t_recv_ns":1000101686,"lat_ns":101686}

events.jsonl:
{"t_ns":1000000000,"node":0,"event":"tx_app","seq":42,"len":1024}
{"t_ns":1000000000,"node":0,"event":"tx_post_egress","seq":42,"len":1024}
{"t_ns":1000050843,"node":1,"event":"rx_nic","seq":42,"len":1024}
{"t_ns":1000050843,"node":1,"event":"rx_post_ingress","seq":42,"len":1024}
```

## 6. Baseline Results Overview

```
┌────────────────────────────────────────────────────────────────┐
│                      Baseline Results                          │
│              (No-op hooks, zero host delay)                    │
└────────────────────────────────────────────────────────────────┘

Packet Size vs Latency:
────────────────────────

    120 μs ┤
           │
    110 μs ┤                              ┌───┐
           │                              │4KB│
    105 μs ┤                              └───┘
           │
    100 μs ┤         ┌────┐  ┌────┐
           │         │1KB │  │256B│
     95 μs ┤         └────┘  └────┘
           │
     90 μs ┤
           └──────────────────────────────────
              pingpong      rpc

All configurations: p50 = p95 = p99 (deterministic)

Outstanding Requests (no effect on tail latency in baseline):
──────────────────────────────────────────────────────────────
- Outstanding = 1:  Same latency (101.69 μs for 1KB)
- Outstanding = 8:  Same latency (101.69 μs for 1KB)
- Outstanding = 32: Same latency (101.69 μs for 1KB)

Expected Latency Breakdown:
────────────────────────────
RTT ≈ 2 × Link Delay + Protocol Overhead
    = 2 × 50 μs + ~2 μs
    = ~102 μs ✓ (matches observed)
```

## 7. Hook Integration Points (Future)

```
┌────────────────────────────────────────────────────────────────┐
│              Model Integration (Future Work)                   │
└────────────────────────────────────────────────────────────────┘

Current (NO-OP):                  Future (With Model):
────────────────                  ─────────────────────

DelayEgress(bytes, seq)          DelayEgress(bytes, seq)
└─► return Time(0)                ├─► Load config
                                  ├─► Calculate delay based on:
                                  │   - bytes
                                  │   - queue state
                                  │   - CPU load model
                                  │   - Random component (seed)
                                  └─► return Time(delay_ns)

DelayIngress(bytes, seq)         DelayIngress(bytes, seq)
└─► return Time(0)                ├─► Load config
                                  ├─► Calculate delay based on:
                                  │   - bytes
                                  │   - buffer state
                                  │   - Interrupt handling
                                  │   - Random component (seed)
                                  └─► return Time(delay_ns)

Integration Process:
────────────────────
1. Implement delay logic in delay_hooks.cc
2. Create model_config.json with parameters
3. Run: ./ns3 run hd_runner -- --hookConfigPath=model_config.json
4. Compare results: baseline (no delay) vs model (with delay)
5. Analyze p99 tail latency changes
```

## 8. Key Metrics & Outputs

```
┌────────────────────────────────────────────────────────────────┐
│                    Output Metrics Summary                      │
└────────────────────────────────────────────────────────────────┘

Per Run:
────────
✓ 10,000 requests completed
✓ 0% packet loss
✓ Latency distribution:
  - p50 (median)
  - p95 (95th percentile)
  - p99 (99th percentile - tail latency)

Aggregate (18 runs):
────────────────────
✓ manifest.csv with all configurations
✓ Summary statistics across workloads
✓ Comparison matrix for:
  - Workload types
  - Outstanding requests
  - Packet sizes

Reproducibility:
────────────────
✓ Same --seed → Identical results
✓ Deterministic network (no random queueing)
✓ p50 = p95 = p99 (no variance in baseline)
```

---

## Summary

This experiment harness provides:
1. **Deterministic baseline** - Clean reference point (no host delay)
2. **Flexible hooks** - Ready for model integration
3. **Comprehensive logging** - Full request lifecycle visibility
4. **Reproducible results** - Seed-based determinism
5. **No harness changes needed** - Model team can plug in directly

The architecture separates concerns cleanly, allowing the model team to focus solely on implementing delay logic without touching the simulation infrastructure.
