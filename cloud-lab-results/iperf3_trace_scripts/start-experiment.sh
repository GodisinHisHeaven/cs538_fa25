#!/bin/bash

# Load configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/experiment_config.sh"

echo "=========================================="
echo "Running Network + STREAM Experiment"
echo "=========================================="
echo ""

# Function to cleanup on exit
cleanup() {
    echo ""
    echo "=========================================="
    echo "Cleaning up..."
    echo "=========================================="
    
    echo "Stopping iperf3 servers on Node 1..."
    ssh -i "$SSH_KEY" "${USER}@${NODE1}" "sudo pkill -9 iperf3 || true" 2>/dev/null
    
    echo "Stopping STREAM instances on Node 1..."
    ssh -i "$SSH_KEY" "${USER}@${NODE1}" "sudo pkill -9 stream || true" 2>/dev/null
    
    echo "Stopping iperf3 clients on Node 0..."
    ssh -i "$SSH_KEY" "${USER}@${NODE0}" "sudo pkill -9 iperf3 || true" 2>/dev/null
    
    echo "Cleanup complete"
}

# Set trap to cleanup on script exit
trap cleanup EXIT INT TERM

echo "Step 1: Starting iperf3 servers on Node 1 (Server)..."
echo "=========================================="
ssh -i "$SSH_KEY" "${USER}@${NODE1}" "cd ${REMOTE_DIR}/server && chmod +x start_server.sh && bash start_server.sh" &
SERVER_PID=$!
sleep 5

echo ""
echo "Step 2: Starting STREAM instances on Node 1 (Server)..."
echo "=========================================="
ssh -i "$SSH_KEY" "${USER}@${NODE1}" "cd ${REMOTE_DIR}/server && chmod +x start_stream.sh && bash start_stream.sh" &
STREAM_PID=$!
sleep 3

echo ""
echo "Step 3: Starting iperf3 clients on Node 0 (Client)..."
echo "=========================================="
ssh -i "$SSH_KEY" "${USER}@${NODE0}" "cd ${REMOTE_DIR}/client && chmod +x start_clients.sh && bash start_clients.sh"
CLIENT_EXIT=$?

echo ""
echo "Step 4: Waiting for STREAM to complete..."
echo "=========================================="
wait $STREAM_PID

echo ""
echo "Step 5: Stopping iperf3 servers..."
echo "=========================================="
ssh -i "$SSH_KEY" "${USER}@${NODE1}" "sudo pkill -2 iperf3 || true"

echo ""
echo "=========================================="
echo "Experiment completed!"
echo "=========================================="
echo ""
echo "Results locations:"
echo "  Node 0 (Client): ${REMOTE_DIR}/logs/iperf3_trace_logs_parallel/"
echo "  Node 1 (Server): ${REMOTE_DIR}/logs/iperf3_server_logs/"
echo "  Node 1 (STREAM): ${REMOTE_DIR}/logs/stream_logs/"
echo ""
echo "To retrieve logs from nodes:"
echo "  scp -i $SSH_KEY -r ${USER}@${NODE0}:${REMOTE_DIR}/logs ./node0_logs"
echo "  scp -i $SSH_KEY -r ${USER}@${NODE1}:${REMOTE_DIR}/logs ./node1_logs"

if [ $CLIENT_EXIT -eq 0 ]; then
    echo ""
    echo "✓ Experiment completed successfully"
    exit 0
else
    echo ""
    echo "✗ Experiment failed (client exit code: $CLIENT_EXIT)"
    exit 1
fi