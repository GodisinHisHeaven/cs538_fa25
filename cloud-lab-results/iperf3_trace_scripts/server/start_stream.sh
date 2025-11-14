#!/usr/bin/env bash
set -euo pipefail

# ---- STREAM Parameters ----
CPU_CORES=(16 20 24 28)           # Cores to pin STREAM instances on
STREAM_BIN="/users/edwinji2/understanding-the-host-network/stream/stream"
NUMA_NODE=0                        # NUMA node for memory binding
OPERATION="ReadWrite64"            # STREAM operation
DURATION=120                       # Duration in seconds

# ---- Output Locations ----
DATE="$(date +%s)"
LOGDIR="/users/edwinji2/understanding-the-host-network/logs/stream_logs"
mkdir -p "$LOGDIR"

# Array to store PIDs
STREAM_PIDS=()

echo "[*] Starting ${#CPU_CORES[@]} STREAM instances..."

# Start all STREAM instances
for CORE in "${CPU_CORES[@]}"; do
    STREAM_LOG="$LOGDIR/stream-$DATE-core$CORE.log"
    
    echo "[*] Starting STREAM on core $CORE..."
    numactl --membind "$NUMA_NODE" --physcpubind "$CORE" \
      "$STREAM_BIN" "$OPERATION" "$DURATION" \
      > "$STREAM_LOG" 2>&1 &
    STREAM_PIDS+=($!)
    
done

echo "[*] All STREAM instances started successfully!"
echo
echo "STREAM instances:"
echo "  Core 16 - PID ${STREAM_PIDS[0]}"
echo "  Core 20 - PID ${STREAM_PIDS[1]}"
echo "  Core 24 - PID ${STREAM_PIDS[2]}"
echo "  Core 28 - PID ${STREAM_PIDS[3]}"
echo
echo "Logs directory: $LOGDIR"
echo "Duration: $DURATION seconds"
echo
echo "To stop all STREAM instances, run:"
echo "  sudo kill ${STREAM_PIDS[@]}"
echo
echo "Or kill them individually by PID"

# Optional: Wait for user interrupt
trap 'echo; echo "[*] Stopping all STREAM instances..."; sudo kill ${STREAM_PIDS[@]} 2>/dev/null; exit 0' INT TERM

# Keep script running (optional - comment out if you want it to exit immediately)
echo "Press Ctrl+C to stop all STREAM instances, or wait for them to complete..."
wait