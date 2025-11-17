#!/bin/bash

# Usage: ./measure_delay.sh <destination_node> <count>
# Example: ./measure_delay.sh nodeB.example.com 10

DESTINATION=$1
COUNT=${2:-5}   # default to 5 pings if not specified

if [ -z "$DESTINATION" ]; then
    echo "Usage: $0 <destination_node> [count]"
    exit 1
fi

echo "Measuring propagation delay to $DESTINATION ($COUNT pings)..."
echo

# Run ping and extract average latency
RESULT=$(ping -c $COUNT $DESTINATION | tail -1 | awk -F '/' '{print $5}')

if [ -z "$RESULT" ]; then
    echo "Failed to get ping results."
    exit 1
fi

echo "Average Propagation Delay: $RESULT ms"

