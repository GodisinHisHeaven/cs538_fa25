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

# Function to run ping on node 1
run_ping() {
    local output_file=$1
    local description=$2
    echo "Starting ping on node 1 ($description)..."
    ssh -i "$SSH_KEY" "${USER}@${NODE1}" \
        "ping -i 0.1 ${NODE0} -c $((DURATION * 10))" \
        > "${output_file}" 2>&1 &
    PING_PID=$!
    echo "Ping started (PID: $PING_PID)"
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

echo ""
echo "=== PHASE 1: Baseline (Ping only) ==="
echo "Running ping without STREAM for ${DURATION} seconds..."
run_ping "${OUTPUT_DIR}/ping_baseline.txt" "baseline"

# Wait for baseline ping to complete
wait $PING_PID
BASELINE_EXIT=$?
echo "Baseline ping completed (exit code: $BASELINE_EXIT)"

# Small pause between experiments
echo ""
echo "Waiting 10 seconds before starting STREAM experiment..."
sleep 10

echo ""
echo "=== PHASE 2: STREAM + Ping ==="
echo "Running STREAM and ping concurrently for ${DURATION} seconds..."

# Start both experiments
run_stream
run_ping "${OUTPUT_DIR}/ping_with_stream.txt" "with STREAM"

# Wait for both processes to complete
wait $STREAM_PID
STREAM_EXIT=$?
wait $PING_PID
PING_EXIT=$?

echo ""
echo "=========================================="
echo "Experiment completed at $(date)"
echo "=========================================="
echo "Baseline ping exit code: $BASELINE_EXIT"
echo "STREAM exit code: $STREAM_EXIT"
echo "Ping with STREAM exit code: $PING_EXIT"
echo ""
echo "Results saved in: $OUTPUT_DIR"
echo "  - ping_baseline.txt (baseline without STREAM)"
echo "  - ping_with_stream.txt (with STREAM running)"
echo "  - stream_output.txt"

# Display summaries
echo ""
echo "=== Baseline Ping Summary ==="
grep -E "rtt|packets" "${OUTPUT_DIR}/ping_baseline.txt" | tail -5

echo ""
echo "=== Ping with STREAM Summary ==="
grep -E "rtt|packets" "${OUTPUT_DIR}/ping_with_stream.txt" | tail -5

echo ""
echo "=== STREAM Summary ==="
tail -20 "${OUTPUT_DIR}/stream_output.txt"

echo ""
echo "=========================================="
echo "Total experiment time: ~$((DURATION * 2 + 10)) seconds ($(((DURATION * 2 + 10) / 60)) minutes)"
echo "=========================================="
