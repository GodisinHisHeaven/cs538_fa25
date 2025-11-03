#!/bin/bash

# Load configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/experiment_config.sh"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTPUT_DIR="./experiment_data_${TIMESTAMP}"

# Create output directory
mkdir -p "$OUTPUT_DIR"

echo "=========================================="
echo "Starting experiment at $(date)"
echo "Data will be saved to: $OUTPUT_DIR"
echo "=========================================="

# Function to start netperf server on node 0
start_netserver() {
    echo "Checking for existing netserver on node 0..."
    # Kill any existing netserver processes
    ssh -i "$SSH_KEY" "${USER}@${NODE0}" \
        "pkill netserver" 2>/dev/null
    sleep 1
    
    echo "Starting netserver on node 0..."
    ssh -i "$SSH_KEY" "${USER}@${NODE0}" \
        "nohup netserver -D > /dev/null 2>&1 &"
    
    sleep 2  # Give server time to start
    
    # Verify netserver is running
    ssh -i "$SSH_KEY" "${USER}@${NODE0}" \
        "pgrep netserver > /dev/null && echo 'Netserver is running' || echo 'Warning: netserver may not be running'"
}

# Function to stop netperf server on node 0
stop_netserver() {
    echo "Stopping netserver on node 0..."
    ssh -i "$SSH_KEY" "${USER}@${NODE0}" \
        "pkill netserver" 2>&1
}

# Function to run netperf from node 1 to node 0
run_netperf() {
    local output_file=$1
    local description=$2
    echo "Starting netperf on node 1 ($description)..."
    ssh -i "$SSH_KEY" "${USER}@${NODE1}" \
        "netperf -H ${NODE0IP} -l ${DURATION} -t TCP_STREAM -- -m 1024" \
        > "${output_file}" 2>&1 &
    NETPERF_PID=$!
    echo "Netperf started (PID: $NETPERF_PID)"
}

# Function to run netperf latency test (TCP_RR for request-response)
run_netperf_latency() {
    local output_file=$1
    local description=$2
    echo "Starting netperf latency test on node 1 ($description)..."
    ssh -i "$SSH_KEY" "${USER}@${NODE1}" \
        "netperf -H ${NODE0IP} -l ${DURATION} -t TCP_RR" \
        > "${output_file}" 2>&1 &
    NETPERF_PID=$!
    echo "Netperf latency test started (PID: $NETPERF_PID)"
}

# Function to run stream on node 0
run_stream() {
    echo "Starting STREAM benchmark on node 0..."
    ssh -i "$SSH_KEY" "${USER}@${NODE0}" \
        "cd ${STREAM_DIR} && ./stream ReadWrite64 ${DURATION}" \
        > "${OUTPUT_DIR}/stream_output.txt" 2>&1 &
    STREAM_PID=$!
    echo "STREAM started (PID: $STREAM_PID)"
}

# Start netserver
start_netserver

echo ""
echo "=== PHASE 1: Baseline (Netperf throughput only) ==="
echo "Running netperf TCP_STREAM without STREAM for ${DURATION} seconds..."
run_netperf "${OUTPUT_DIR}/netperf_baseline_throughput.txt" "baseline throughput"

# Wait for baseline netperf to complete
wait $NETPERF_PID
BASELINE_THROUGHPUT_EXIT=$?
echo "Baseline netperf throughput completed (exit code: $BASELINE_THROUGHPUT_EXIT)"

# Small pause between experiments
echo ""
echo "Waiting 10 seconds before starting latency baseline..."
sleep 10

echo ""
echo "=== PHASE 2: Baseline (Netperf latency only) ==="
echo "Running netperf TCP_RR without STREAM for ${DURATION} seconds..."
run_netperf_latency "${OUTPUT_DIR}/netperf_baseline_latency.txt" "baseline latency"

# Wait for baseline latency test to complete
wait $NETPERF_PID
BASELINE_LATENCY_EXIT=$?
echo "Baseline netperf latency completed (exit code: $BASELINE_LATENCY_EXIT)"

# Small pause between experiments
echo ""
echo "Waiting 10 seconds before starting STREAM experiment..."
sleep 10

echo ""
echo "=== PHASE 3: STREAM + Netperf throughput ==="
echo "Running STREAM and netperf TCP_STREAM concurrently for ${DURATION} seconds..."

# Start both experiments
run_stream
run_netperf "${OUTPUT_DIR}/netperf_with_stream_throughput.txt" "with STREAM throughput"

# Wait for both processes to complete
wait $STREAM_PID
STREAM_EXIT=$?
wait $NETPERF_PID
NETPERF_THROUGHPUT_EXIT=$?

echo "STREAM completed (exit code: $STREAM_EXIT)"
echo "Netperf throughput with STREAM completed (exit code: $NETPERF_THROUGHPUT_EXIT)"

# Small pause between experiments
echo ""
echo "Waiting 10 seconds before starting final latency test..."
sleep 10

echo ""
echo "=== PHASE 4: STREAM + Netperf latency ==="
echo "Running STREAM and netperf TCP_RR concurrently for ${DURATION} seconds..."

# Start both experiments
run_stream
run_netperf_latency "${OUTPUT_DIR}/netperf_with_stream_latency.txt" "with STREAM latency"

# Wait for both processes to complete
wait $STREAM_PID
STREAM_EXIT2=$?
wait $NETPERF_PID
NETPERF_LATENCY_EXIT=$?

echo "STREAM completed (exit code: $STREAM_EXIT2)"
echo "Netperf latency with STREAM completed (exit code: $NETPERF_LATENCY_EXIT)"

# Stop netserver
stop_netserver

echo ""
echo "=========================================="
echo "Experiment completed at $(date)"
echo "=========================================="
echo "Baseline netperf throughput exit code: $BASELINE_THROUGHPUT_EXIT"
echo "Baseline netperf latency exit code: $BASELINE_LATENCY_EXIT"
echo "STREAM exit code (throughput test): $STREAM_EXIT"
echo "Netperf throughput with STREAM exit code: $NETPERF_THROUGHPUT_EXIT"
echo "STREAM exit code (latency test): $STREAM_EXIT2"
echo "Netperf latency with STREAM exit code: $NETPERF_LATENCY_EXIT"
echo ""
echo "Results saved in: $OUTPUT_DIR"
echo "  - netperf_baseline_throughput.txt (baseline throughput without STREAM)"
echo "  - netperf_baseline_latency.txt (baseline latency without STREAM)"
echo "  - netperf_with_stream_throughput.txt (throughput with STREAM running)"
echo "  - netperf_with_stream_latency.txt (latency with STREAM running)"
echo "  - stream_output.txt"

# Display summaries
echo ""
echo "=== Baseline Netperf Throughput Summary ==="
tail -10 "${OUTPUT_DIR}/netperf_baseline_throughput.txt"

echo ""
echo "=== Baseline Netperf Latency Summary ==="
tail -10 "${OUTPUT_DIR}/netperf_baseline_latency.txt"

echo ""
echo "=== Netperf Throughput with STREAM Summary ==="
tail -10 "${OUTPUT_DIR}/netperf_with_stream_throughput.txt"

echo ""
echo "=== Netperf Latency with STREAM Summary ==="
tail -10 "${OUTPUT_DIR}/netperf_with_stream_latency.txt"

echo ""
echo "=== STREAM Summary ==="
tail -20 "${OUTPUT_DIR}/stream_output.txt"

echo ""
echo "=========================================="
echo "Total experiment time: ~$((DURATION * 4 + 40)) seconds ($(((DURATION * 4 + 40) / 60)) minutes)"
echo "=========================================="