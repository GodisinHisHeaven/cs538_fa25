#!/usr/bin/env bash
set -euo pipefail

# ---- Server Parameters ----
CPU_CORES=(0 4 8 12)              # Cores to pin iperf3 servers on
BASE_PORT=5200
INTERVAL=30                        # Reporting interval in seconds
FORMAT="m"                         # Format (m = Mbits/sec)

# ---- Output Locations ----
DATE="$(date +%s)"
LOGDIR="/users/edwinji2/understanding-the-host-network/logs/iperf3_server_logs"
mkdir -p "$LOGDIR"

# Array to store PIDs
SERVER_PIDS=()

echo "[*] Starting ${#CPU_CORES[@]} iperf3 server instances..."

# Start all iperf3 server instances
PORT_OFFSET=0
for CORE in "${CPU_CORES[@]}"; do
    PORT=$((BASE_PORT + PORT_OFFSET))
    SERVER_LOG="$LOGDIR/iperf3-server-$DATE-core$CORE-port$PORT.log"
    
    echo "[*] Starting iperf3 server on core $CORE, port $PORT..."
    sudo taskset -c $CORE nice -n -20 \
      iperf3 -s --port "$PORT" -i "$INTERVAL" -f "$FORMAT" \
      > "$SERVER_LOG" 2>&1 &
    SERVER_PIDS+=($!)
    
    PORT_OFFSET=$((PORT_OFFSET + 1))
    sleep 0.5   # stagger starts slightly
done

echo "[*] All iperf3 servers started successfully!"
echo
echo "Server instances:"
echo "  Core 0  - Port 5200 - PID ${SERVER_PIDS[0]}"
echo "  Core 4  - Port 5201 - PID ${SERVER_PIDS[1]}"
echo "  Core 8  - Port 5202 - PID ${SERVER_PIDS[2]}"
echo "  Core 12 - Port 5203 - PID ${SERVER_PIDS[3]}"
echo
echo "Logs directory: $LOGDIR"
echo
echo "To stop all servers, run:"
echo "  sudo kill ${SERVER_PIDS[@]}"
echo
echo "Or kill them individually by PID"

# Optional: Wait for user interrupt
trap 'echo; echo "[*] Stopping all servers..."; sudo kill ${SERVER_PIDS[@]} 2>/dev/null; exit 0' INT TERM

# Keep script running (optional - comment out if you want it to exit immediately)
echo "Press Ctrl+C to stop all servers..."
wait