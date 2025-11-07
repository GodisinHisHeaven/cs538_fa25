# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a CS538 course project studying host-induced delays in datacenter networks. The repository contains two complementary experimental approaches:

**1. ns-3 Simulation-Based Experiments** (`ns-3/` directory)
- Deterministic network simulator for controlled experiments
- Custom experiment harness in `ns-3/scratch/hd_runner/`
- Point-to-point topology: Client (Host0) ↔ Server (Host1)
- Delay hooks for future model integration (currently no-op baseline)
- Comprehensive logging and statistics generation

**2. CloudLab Real Hardware Experiments** (`cloud-lab-results/` directory)
- Real hardware experiments on CloudLab infrastructure
- Measures impact of CPU/memory load (via STREAM benchmark) on network latency
- Uses ping and netperf to measure latency and throughput
- Compares baseline (no load) vs loaded (STREAM running) scenarios
- Includes collected experimental data

## Building and Running

### Part 1: ns-3 Simulation Experiments

#### Initial Setup

The ns-3 project uses a custom build system. ns-3 must be configured and built before running experiments.

```bash
# Navigate to ns-3 directory
cd ns-3

# Configure ns-3 with examples enabled (first time only)
./ns3 configure --enable-examples

# Build the hd_runner experiment
./ns3 build hd_runner
```

#### Running Single Experiments

```bash
# From ns-3 directory
./ns3 run hd_runner -- --nReq=10000 --workload=rpc --outstanding=8 --reqBytes=1024 --rspBytes=1024
```

**Important:** The `--` separator is required between ns3 arguments and hd_runner arguments.

#### Running Full Experiment Matrix

```bash
# From ns-3/scratch directory
cd scratch
python3 run_matrix.py
```

This runs 18 baseline experiments across:
- Workloads: pingpong, rpc
- Outstanding requests: 1, 8, 32
- Packet sizes: 256B, 1KB, 4KB

#### Generating Results Manifest

After running experiments:

```bash
# From ns-3/scratch directory
python3 generate_manifest.py
```

This creates `out/sim/manifest.csv` aggregating all run results.

### Part 2: CloudLab Real Hardware Experiments

The `cloud-lab-results/` directory contains scripts for running real hardware experiments on CloudLab.

#### Prerequisites

- CloudLab account with active experiment
- Two nodes allocated (configuration in `experiment_config.sh`)
- SSH key configured for accessing nodes

#### Setup CloudLab Nodes

```bash
cd cloud-lab-results

# Edit experiment_config.sh with your node details:
# - NODE0, NODE1: CloudLab hostnames
# - NODE0IP, NODE1IP: Internal network IPs
# - SSH_KEY: Path to your SSH key
# - USER: Your CloudLab username

# Setup both nodes (installs netperf, clones STREAM repo, compiles)
./setup.sh
```

#### Running CloudLab Experiments

**Ping Experiment** (measures ICMP latency with and without CPU load):
```bash
./stream_ping_experiment.sh
```

This runs:
1. Baseline: Ping only (5 minutes)
2. Loaded: Ping + STREAM benchmark running concurrently (5 minutes)

**Netperf Experiment** (measures TCP throughput and latency):
```bash
./stream_netperf_experiment.sh
```

This runs:
1. Baseline throughput (TCP_STREAM)
2. Loaded throughput (TCP_STREAM + STREAM benchmark)
3. Baseline latency (TCP_RR - request-response)
4. Loaded latency (TCP_RR + STREAM benchmark)

#### CloudLab Output Structure

Each run creates: `experiment_data_<timestamp>/`
- `ping_baseline.txt`: Ping results without STREAM
- `ping_with_stream.txt`: Ping results with STREAM running
- `stream_output.txt`: STREAM benchmark output
- `netperf_baseline_throughput.txt`: Baseline throughput test
- `netperf_with_stream_throughput.txt`: Throughput with CPU load
- `netperf_baseline_latency.txt`: Baseline latency test
- `netperf_with_stream_latency.txt`: Latency with CPU load

## Code Architecture

### Core Components

**1. Experiment Harness (`ns-3/scratch/hd_runner/hd_runner.cc`)**
- Main simulation runner
- Implements RpcClientApp and RpcServerApp custom applications
- Handles request/response lifecycle tracking
- Generates output files (config.json, rpc.jsonl, events.jsonl, summary.txt)

**2. Delay Hooks (`ns-3/scratch/hd_runner/delay_hooks.h` and `delay_hooks.cc`)**
- Interface for host-delay modeling
- `DelayEgress()`: Called before NIC transmission
- `DelayIngress()`: Called before application delivery
- Currently returns zero delay (no-op baseline)
- Ready for model integration via `--hookConfigPath`

**3. Orchestration Scripts**
- `run_matrix.py`: Runs full experiment matrix
- `generate_manifest.py`: Aggregates results into manifest.csv

### Data Flow

```
Client App → DelayEgress Hook → NIC Tx → Network → NIC Rx → DelayIngress Hook → Server App
                                                                                      ↓
                                                                              Send Response
                                                                                      ↓
Server App → DelayEgress Hook → NIC Tx → Network → NIC Rx → DelayIngress Hook → Client App
```

### Key Classes and Functions

- `RpcClientApp`: Custom application that sends requests and tracks RTT
  - `SendRequest()`: Initiates request with delay hook
  - `HandleResponse()`: Receives response, logs latency

- `RpcServerApp`: Simple echo server that responds immediately
  - `HandleRequest()`: Processes request and sends response

- `DelayHooks` class: Static methods for delay injection
  - `Initialize()`: Sets up hooks with config (realistic/moderate/severe profiles)
  - `DelayEgress()`, `DelayIngress()`: Cache miss + queueing delay model implementation
  - `CalculateLoad()`: Rolling window packet rate tracking
  - `CalculateCacheMisses()`: Probabilistic cache miss estimation
  - `CalculatePenalty()`, `CalculateQueueDelay()`: Step functions for load-dependent delays

### Host Delay Model (Implemented)

**Model Formula:**
```
Host_delay = base_CPU_cycles + (cache_misses × penalty_P(L)) + queueing_delay_Q(L)
```

**Components:**
- Base CPU cycles: 150ns
- Cache misses: Probabilistic (10% for ≤256B, 40% for 256B-1KB, 70% for >1KB)
- Penalty P(L): Step function at load=0.7 (100ns → 300ns per miss)
- Queue delay Q(L): Step function at load=0.6 (50ns base, grows with load)
- Load factor L: From rolling 100ms window packet rate

**Configuration Profiles** (via `--hookConfigPath`):
- `realistic` (default): severity=1.0, ~1-2μs impact (matches CloudLab)
- `moderate`: severity=3.0, ~5-15μs impact (visible tail latency)
- `severe`: severity=10.0, ~20-50μs impact (extreme scenarios)

**Example Usage:**
```bash
# Test with moderate config and high load
./ns3 run hd_runner -- --nReq=10000 --outstanding=32 --hookConfigPath=moderate

# Results: p50=115μs, p99=125μs (vs baseline p50=p99=102μs)
```

### Logging Infrastructure

**Per-Request Tracking (`rpc.jsonl`):**
```json
{"seq":42,"t_send_ns":1234567890,"t_recv_ns":1235567890,"lat_ns":1000000}
```

**Event Timeline (`events.jsonl`):**
```json
{"t_ns":1234567890,"node":0,"event":"tx_app","seq":42,"len":1024}
{"t_ns":1234567890,"node":0,"event":"tx_post_egress","seq":42,"len":1024}
```

**Summary Statistics (`summary.txt`):**
- Configuration details
- Latency percentiles (p50, p95, p99)
- Both nanosecond and microsecond units

## Command-Line Options

Key hd_runner options:
- `--linkRate=10Gbps`: Link data rate
- `--linkDelay=50us`: Link propagation delay
- `--workload=rpc`: Workload type (pingpong|rpc)
- `--nReq=10000`: Number of requests
- `--outstanding=8`: Outstanding requests (concurrency)
- `--reqBytes=1024`: Request size in bytes
- `--rspBytes=1024`: Response size in bytes
- `--enableEgressHook=1`: Enable egress delay hook
- `--enableIngressHook=1`: Enable ingress delay hook
- `--hookConfigPath=""`: Delay model profile: "" or "realistic" (default), "moderate", or "severe"
- `--seed=1`: Random seed for determinism
- `--outDir=out/sim`: Output directory

## Output Structure

Each run creates: `out/sim/<run-id>/`
- `config.json`: Complete configuration
- `rpc.jsonl`: Per-request latency records
- `events.jsonl`: Event timeline
- `summary.txt`: Human-readable summary with percentiles

After matrix runs: `out/sim/manifest.csv` contains all results.

## Development Notes

### ns-3 Build System

- ns-3 uses CMake with a custom `ns3` wrapper tool
- The wrapper provides a Waf-like API
- Custom experiments go in `ns-3/scratch/` directory
- Each subdirectory with .cc files becomes a runnable target
- Use `./ns3 build <target>` to build specific experiments

### Testing Changes

```bash
# Build after code changes
cd ns-3
./ns3 build hd_runner

# Run a quick test
./ns3 run hd_runner -- --nReq=100 --workload=rpc --outstanding=1

# Check output
cat out/sim/<run-id>/summary.txt
```

### Delay Model Implementation Details

The delay model is fully implemented in `delay_hooks.cc`:

**Current Implementation:**
- Cache miss + queueing delay model with load-dependent step functions
- Three configuration profiles: realistic, moderate, severe
- Rolling window load tracking (100ms window, per-node)
- Probabilistic cache miss estimation based on packet size
- Step functions activate at load thresholds (0.6 for queue, 0.7 for penalty)

**To modify or extend the model:**
1. Edit `delay_hooks.cc` - update model parameters or add new components
2. Test changes: `./ns3 build hd_runner && ./ns3 run hd_runner -- --nReq=1000 --hookConfigPath=moderate`
3. Verify model behavior in output summary (p50/p95/p99 latencies)
4. Check events.jsonl for per-packet delay components if needed

**Configuration profiles** can be edited by modifying `CreateRealisticConfig()`, `CreateModerateConfig()`, and `CreateSevereConfig()` functions in delay_hooks.cc.

No changes to `hd_runner.cc` are required for model modifications.

### Code Style

The codebase follows ns-3's GNU coding style:
- Indentation: 4 spaces (configured in files with `indent-tabs-mode:nil`)
- Naming: CamelCase for classes, camelCase for methods, snake_case for variables
- Header format: `/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */`

## Baseline Results

With no-op hooks (zero host delay):
- Median RTT ≈ 100-107μs (2 × 50μs link delay + protocol overhead)
- Deterministic: p50 = p95 = p99 (no variance)
- Packet size affects serialization time slightly (256B: 100.46μs, 4KB: 106.67μs)
- Outstanding requests don't affect tail latency in baseline (no queueing)

## Known Issues

1. **Workload Implementation**: Both "pingpong" and "rpc" currently use the same RPC implementation
2. **QDisc Support**: Only "none" is tested; FQ-CoDel is plumbed but not validated
3. **Event Logging**: Always enabled; could add flag to disable for performance
4. **Topology Simplification**: Uses direct P2P link instead of explicit switch node

## Important Paths

### ns-3 Simulation
- Main experiment: `ns-3/scratch/hd_runner/hd_runner.cc`
- Delay hooks: `ns-3/scratch/hd_runner/delay_hooks.{h,cc}`
- Orchestration: `ns-3/scratch/run_matrix.py`
- Documentation: `ns-3/scratch/hd_runner/README.md`, `EXPERIMENT_DIAGRAM.md`
- Output directory: `ns-3/out/sim/` (or `ns-3/scratch/out/sim/`)

### CloudLab Experiments
- Configuration: `cloud-lab-results/experiment_config.sh`
- Setup script: `cloud-lab-results/setup.sh`
- Ping experiment: `cloud-lab-results/stream_ping_experiment.sh`
- Netperf experiment: `cloud-lab-results/stream_netperf_experiment.sh`
- Collected data: `cloud-lab-results/experiment_data_*/`

## ns-3 Specific Notes

- ns-3 version in use: Check `ns-3/README.md` for version info
- ns-3 uses discrete-event simulation
- Time is represented in nanosecond precision
- Applications are installed on nodes with start/stop times
- Logging uses NS_LOG_* macros (can be enabled with NS_LOG environment variable)

## CloudLab Experiments Notes

### Experimental Design

The CloudLab experiments measure the impact of CPU/memory load on network latency:

**STREAM Benchmark**: Memory bandwidth benchmark that creates CPU and memory subsystem load
- Modified to use STREAM_ARRAY_SIZE of 15,000,000 (larger than cache)
- Runs ReadWrite64 pattern for continuous memory stress
- Repository: https://github.com/host-architecture/understanding-the-host-network

**Network Measurements**:
- **Ping**: ICMP echo requests at 10Hz (100ms interval) for 5 minutes
- **Netperf TCP_STREAM**: Throughput test with 1KB messages
- **Netperf TCP_RR**: Request-response latency test

### Experiment Phases

Each experiment compares:
1. **Baseline**: Network test alone (no CPU load)
2. **Loaded**: Network test + STREAM running concurrently

This measures how host CPU/memory contention affects network performance.

### Key Configuration

- Two CloudLab nodes on internal network (10.10.1.0/24)
- Uses internal IPs to avoid external network interference
- 5-minute test duration per phase
- SSH-based orchestration from control machine

### Analyzing Results

Results are in human-readable text format:
- Ping results: Look for RTT min/avg/max/mdev statistics
- Netperf throughput: Throughput in Mbps or transactions/sec
- Netperf latency: Latency percentiles in microseconds
- Compare baseline vs loaded to quantify host delay impact
