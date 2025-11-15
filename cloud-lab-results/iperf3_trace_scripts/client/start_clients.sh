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
    PCAP="$LOGDIR/trace-$DATE-core$CORE-port$PORT.pcap"
    IPERF_LOG="$LOGDIR/iperf-$DATE-core$CORE-port$PORT.log"
    
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

# Stop all tcpdump captures
echo "[*] Stopping packet captures..."
for PID in "${TCPDUMP_PIDS[@]}"; do
    sudo kill -2 "$PID" 2>/dev/null || true  # SIGINT ensures pcap closes cleanly
done

# Wait a moment for tcpdump to finish writing
sleep 2

echo "[*] Done."
echo "Results saved to: $LOGDIR"
echo "  Core 0 (Port 5201): trace-$DATE-core0-port5201.pcap, iperf-$DATE-core0-port5201.log"
echo "  Core 4 (Port 5202): trace-$DATE-core4-port5202.pcap, iperf-$DATE-core4-port5202.log"
echo "  Core 8 (Port 5203): trace-$DATE-core8-port5203.pcap, iperf-$DATE-core8-port5203.log"
echo "  Core 12 (Port 5204): trace-$DATE-core12-port5204.pcap, iperf-$DATE-core12-port5204.log"
echo
echo "Now run: python3 parse_pcap.py $LOGDIR/trace-$DATE-core*-port*.pcap"