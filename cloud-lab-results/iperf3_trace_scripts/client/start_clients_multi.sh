#!/usr/bin/env bash
set -euo pipefail

# ---- Mode Configuration ----
MODE="${1:-baseline}"              # baseline or loaded (default: baseline)

if [[ "$MODE" != "baseline" && "$MODE" != "loaded" ]]; then
    echo "Error: Invalid mode '$MODE'. Use 'baseline' or 'loaded'."
    echo "Usage: $0 [baseline|loaded]"
    exit 1
fi

echo "[*] Running in $MODE mode"

# ---- Experiment Parameters ----
IF="ens2f0np0"                    # Network interface on the client
SERVER_IP="10.10.1.1"             # iperf3 server address
BASE_PORT=5201
SIZE=75G                           # Size of transfer
CPU_CORES=(0 4 8 12)              # Cores to pin iperf3 instances on

# ---- Output Locations ----
DATE="$(date +%s)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGDIR="$SCRIPT_DIR/logs"
mkdir -p "$LOGDIR"

# Array to store PIDs
TCPDUMP_PIDS=()
IPERF_PIDS=()

sudo pkill -9 -f iperf || true #kill existing iperf servers/clients
sleep 1

echo "[*] Starting ${#CPU_CORES[@]} iperf3 instances..."

# Start tcpdump captures and iperf3 instances for each core
PORT_OFFSET=0
for CORE in "${CPU_CORES[@]}"; do
    PORT=$((BASE_PORT + PORT_OFFSET))
    PCAP="$LOGDIR/${MODE}-trace-$DATE-core$CORE-port$PORT.pcap"
    IPERF_LOG="$LOGDIR/${MODE}-iperf-$DATE-core$CORE-port$PORT.log"
    
    echo "[*] Starting packet capture for core $CORE on port $PORT..."
    sudo tcpdump -i "$IF" -s 128 tcp and host "$SERVER_IP" and port "$PORT" -w "$PCAP" &
    TCPDUMP_PIDS+=($!)
    
    PORT_OFFSET=$((PORT_OFFSET + 1))
    sleep 1   # stagger starts slightly
done

sleep 4   # allow all captures to initialize

# Start all iperf3 instances
PORT_OFFSET=0
for CORE in "${CPU_CORES[@]}"; do
    PORT=$((BASE_PORT + PORT_OFFSET))
    IPERF_LOG="$LOGDIR/${MODE}-iperf-$DATE-core$CORE-port$PORT.log"
    
    echo "[*] Running iperf3 on core $CORE with port $PORT..."
    sudo taskset -c $CORE nice -n -20 \
      iperf3 -c "$SERVER_IP" -p "$PORT" -n "$SIZE" \
      -C dctcp --logfile "$IPERF_LOG" &
    IPERF_PIDS+=($!)
    
    PORT_OFFSET=$((PORT_OFFSET + 1))
done

# Wait for all iperf3 instances to complete
echo "[*] Waiting for iperf3 instances to complete..."
for PID in "${IPERF_PIDS[@]}"; do
    wait "$PID"
done

# Stop all tcpdump captures
echo "[*] Stopping packet captures..."
for PID in "${TCPDUMP_PIDS[@]}"; do
    sudo kill -2 "$PID" 2>/dev/null || true  # SIGINT ensures pcap closes cleanly
done

# Wait a moment for tcpdump to finish writing
sleep 2

echo "[*] Done."
echo "Results saved to: $LOGDIR"
PORT_OFFSET=0
for CORE in "${CPU_CORES[@]}"; do
    PORT=$((BASE_PORT + PORT_OFFSET))
    echo "  Core $CORE (Port $PORT): ${MODE}-trace-$DATE-core$CORE-port$PORT.pcap, ${MODE}-iperf-$DATE-core$CORE-port$PORT.log"
    PORT_OFFSET=$((PORT_OFFSET + 1))
done