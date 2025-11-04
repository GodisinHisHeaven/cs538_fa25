#!/bin/bash

# Load configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/experiment_config.sh"

echo "=========================================="
echo "Setting up CloudLab nodes"
echo "=========================================="

# Function to setup a node
setup_node() {
    local node=$1
    local node_name=$2
    
    echo ""
    echo "=== Setting up $node_name ($node) ==="
    echo ""
    
    ssh -i "$SSH_KEY" "${USER}@${node}" bash << 'ENDSSH'
        set -e  # Exit on error
        
        echo "Step 1: Updating package list..."
        sudo apt update
        
        echo "Step 2: Installing netperf..."
        sudo apt install -y netperf
        
        echo "Step 3: Cloning repository..."
        if [ -d "~/understanding-the-host-network" ]; then
            echo "Repository already exists, pulling latest changes..."
            cd ~/understanding-the-host-network
            git pull
        else
            git clone https://github.com/host-architecture/understanding-the-host-network.git
        fi
        
        echo "Step 4: Modifying STREAM_ARRAY_SIZE to 15000000..."
        cd ~/understanding-the-host-network/stream
        sed -i 's/STREAM_ARRAY_SIZE\t10000000/STREAM_ARRAY_SIZE\t15000000/' stream.c
        
        echo "Step 5: Compiling STREAM..."
        make clean 2>/dev/null || true
        make
        
        echo "Step 6: Testing STREAM (10 second run)..."
        ./stream ReadWrite64 10
        
        if [ $? -eq 0 ]; then
            echo ""
            echo "✓ Setup completed successfully!"
        else
            echo ""
            echo "✗ STREAM test failed!"
            exit 1
        fi
ENDSSH
    
    if [ $? -eq 0 ]; then
        echo ""
        echo "$node_name setup completed successfully"
    else
        echo ""
        echo "$node_name setup failed"
        exit 1
    fi
}

# Setup both nodes
setup_node "$NODE0" "Node 0"
setup_node "$NODE1" "Node 1"

echo ""
echo "=========================================="
echo "Both nodes are ready for experiments!"
echo "=========================================="
echo ""
echo "You can now run:"
echo "  - ./stream_netperf_experiment.sh"
echo "  - ./stream_ping_experiment.sh"