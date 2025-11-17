#!/usr/bin/env bash
set -euo pipefail

# ---- Experiment Parameters ----
IF="ens2f0np0"                    # Network interface on the client
SERVER_IP="10.10.1.1"             # iperf3 server address
BASE_PORT=5201
TIME=30                            # Duration of test (seconds)
CPU_CORES=(0 4 8 12)              # Cores to pin iperf3 instances on

# ---- Output Locations ----
DATE="$(date +%s)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGDIR="$SCRIPT_DIR/logs"
mkdir -p "$LOGDIR"

# Single pcap file for all traffic
PCAP="$LOGDIR/trace-$DATE-all-ports.pcap"

# Array to store PIDs
IPERF_PIDS=()

sudo pkill -9 -f iperf || true #kill existing iperf servers/clients
sleep 1

echo "[*] Starting ${#CPU_CORES[@]} iperf3 instances..."

# Build port range filter for tcpdump
PORTS=()
PORT_OFFSET=0
for CORE in "${CPU_CORES[@]}"; do
    PORT=$((BASE_PORT + PORT_OFFSET))
    PORTS+=($PORT)
    PORT_OFFSET=$((PORT_OFFSET + 1))
done

# Create tcpdump filter: "tcp and host SERVER_IP and (port P1 or port P2 or ...)"
FILTER="tcp and host $SERVER_IP and (port ${PORTS[0]}"
for ((i=1; i<${#PORTS[@]}; i++)); do
    FILTER="$FILTER or port ${PORTS[$i]}"
done
FILTER="$FILTER)"

echo "[*] Starting single packet capture for all ports..."
echo "    Filter: $FILTER"
sudo tcpdump -i "$IF" -s 128 "$FILTER" -w "$PCAP" &
TCPDUMP_PID=$!

sleep 4   # allow capture to initialize

# Start all iperf3 instances
PORT_OFFSET=0
for CORE in "${CPU_CORES[@]}"; do
    PORT=$((BASE_PORT + PORT_OFFSET))
    IPERF_LOG="$LOGDIR/iperf-$DATE-core$CORE-port$PORT.log"
    
    echo "[*] Running iperf3 on core $CORE with port $PORT..."
    sudo taskset -c $CORE nice -n -20 \
      iperf3 -c "$SERVER_IP" -p "$PORT" -t "$TIME" \
      -C dctcp --logfile "$IPERF_LOG" &
    IPERF_PIDS+=($!)
    
    PORT_OFFSET=$((PORT_OFFSET + 1))
done

# Wait for all iperf3 instances to complete
echo "[*] Waiting for iperf3 instances to complete..."
for PID in "${IPERF_PIDS[@]}"; do
    wait "$PID"
done

# Stop tcpdump capture
echo "[*] Stopping packet capture..."
sudo kill -2 "$TCPDUMP_PID" 2>/dev/null || true  # SIGINT ensures pcap closes cleanly

# Wait a moment for tcpdump to finish writing
sleep 2

echo "[*] Done."
echo "Results saved to: $LOGDIR"
echo ""
echo "Packet capture: $PCAP"
echo ""
echo "iperf3 logs:"
PORT_OFFSET=0
for CORE in "${CPU_CORES[@]}"; do
    PORT=$((BASE_PORT + PORT_OFFSET))
    echo "  Core $CORE (Port $PORT): iperf-$DATE-core$CORE-port$PORT.log"
    PORT_OFFSET=$((PORT_OFFSET + 1))
done
echo