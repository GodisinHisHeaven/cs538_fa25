# sudo apt update
# sudo apt-get install msr-tools 
# sudo apt-get install libpci-dev

# git clone https://github.com/host-architecture/understanding-the-host-network.git
# cd ~/understanding-the-host-network/stream
# make clean 2>/dev/null || true 
# make 
# ./stream ReadWrite64 10


# cd ~
# git clone https://github.com/aliireza/ddio-bench.git
# git clone https://github.com/Terabit-Ethernet/Understanding-network-stack-overheads-SIGCOMM-2021.git
# git clone https://github.com/Terabit-Ethernet/hostCC.git
# cd ddio-bench

# vi change-ddio.c # Change the bus
# gcc change-ddio.c -o change-ddio -lpci sudo ./change-ddio

# mkdir /tmp/mlnx-tools
# cd /tmp/mlnx-tools
# wget https://github.com/Mellanox/mlnx-tools/archive/refs/tags/v24.10.1.tar.gz
# tar -xvf v24.10.1.tar.gz mlnx-tools-24.10.1/
# cd /users/edwinji2/mlnx-tools-24.10.1
# sudo make install

# # sudo mlnx_qos -i ens2f0np0 --pfc 0,0,0,0,0,0,0,0
# # cd /users/edwinji2/hostCC/utils
# # chmod +x ./setup-envir.sh

# sudo ./setup-envir.sh -H /users/edwinji2 -i ens2f0np0 -a 10.10.1.1 -m 4000 -o 1 -d 0 -f 1 -r 0 -p 0
# sudo ./setup-envir.sh -H /users/edwinji2 -i ens2f0np0 -a 10.10.1.2 -m 4000 -o 1 -d 0 -f 1 -r 0 -p 0
# cd /users/edwinji2/hostCC/utils/tcp
# chmod +x ./run-netapp-tput.sh
# sudo ./run-netapp-tput.sh -m server -o test -S 4 -c 0,4,8,12
# sudo ./run-netapp-tput.sh -m client -o test -S 4 -C 4 -a 10.10.1.1 -c 0,4,8,12


#!/bin/bash

# Load configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/experiment_config.sh"

# Set default remote directory if not specified in config
REMOTE_DIR="${REMOTE_DIR:-~}"

echo "=========================================="
echo "Copying folders to CloudLab nodes"
echo "=========================================="
echo "Remote directory: $REMOTE_DIR"
echo ""

# Function to install dependencies on a node
install_dependencies() {
    local node=$1
    local node_name=$2
    
    echo ""
    echo "=== Installing dependencies on $node_name ($node) ==="
    echo ""
    
    # Copy mlnx-tools tarball to the node
    echo "Copying mlnx-tools tarball..."
    scp -i "$SSH_KEY" "${SCRIPT_DIR}/v24.10.1.tar.gz" "${USER}@${node}:/tmp/"
    
    ssh -i "$SSH_KEY" "${USER}@${node}" bash << 'ENDSSH'
        set -e  # Exit on error
        
        echo "Step 1: Updating package list..."
        sudo apt update
        
        echo "Step 2: Installing msr-tools..."
        sudo apt-get install -y msr-tools
        
        echo "Step 3: Installing libpci-dev..."
        sudo apt-get install -y libpci-dev
        
        echo "Step 4: Creating python symlink for mlnx-tools..."
        if [ ! -f /usr/bin/python ]; then
            sudo ln -s /usr/bin/python3 /usr/bin/python
            echo "Created symlink: /usr/bin/python -> /usr/bin/python3"
        else
            echo "Python symlink already exists"
        fi
        
        echo "Step 5: Installing mlnx-tools..."
        mkdir -p /tmp/mlnx-tools
        cd /tmp/mlnx-tools
        tar -xvf /tmp/v24.10.1.tar.gz
        cd mlnx-tools-24.10.1
        
        echo "Installing mlnx-tools to /usr/bin..."
        sudo make install
        
        # Verify installation
        if [ -f /usr/bin/mlnx_qos ]; then
            echo "mlnx_qos installed successfully"
        else
            echo "Warning: mlnx_qos not found in /usr/bin"
            echo "Checking alternative locations..."
            sudo find /usr -name "mlnx_qos" 2>/dev/null || echo "mlnx_qos not found"
        fi
        
        echo ""
        echo "✓ Dependencies installed successfully!"
ENDSSH
    
    if [ $? -eq 0 ]; then
        echo ""
        echo "$node_name dependencies installed successfully"
    else
        echo ""
        echo "$node_name dependency installation failed"
        exit 1
    fi
}

# Function to copy files to a node
copy_to_node() {
    local node=$1
    local node_name=$2
    local folders=("${@:3}")  # All arguments from 3rd onward
    
    echo ""
    echo "=== Copying to $node_name ($node) ==="
    echo ""
    
    # Copy setup-env.sh first
    echo "Copying setup-env.sh..."
    scp -i "$SSH_KEY" "${SCRIPT_DIR}/setup-env.sh" "${USER}@${node}:${REMOTE_DIR}/"
    
    # Copy each specified folder
    for folder in "${folders[@]}"; do
        if [ -d "${SCRIPT_DIR}/${folder}" ]; then
            echo "Copying ${folder} folder..."
            scp -i "$SSH_KEY" -r "${SCRIPT_DIR}/${folder}" "${USER}@${node}:${REMOTE_DIR}/"
        else
            echo "Warning: ${folder} folder not found at ${SCRIPT_DIR}/${folder}"
        fi
    done
    
    if [ $? -eq 0 ]; then
        echo ""
        echo "✓ Files copied to $node_name successfully"
    else
        echo ""
        echo "✗ Failed to copy files to $node_name"
        exit 1
    fi
}

# Verify local folders exist
echo "Verifying local folders..."
missing_folders=()

for folder in "client" "server" "stream" "setup-env.sh" "v24.10.1.tar.gz"; do
    if [ ! -e "${SCRIPT_DIR}/${folder}" ]; then
        missing_folders+=("$folder")
    fi
done

if [ ${#missing_folders[@]} -gt 0 ]; then
    echo "Error: Missing required files/folders:"
    for folder in "${missing_folders[@]}"; do
        echo "  - $folder"
    done
    exit 1
fi

echo "All local folders found"
echo ""

# Install dependencies on both nodes first
install_dependencies "$NODE0" "Node 0"
install_dependencies "$NODE1" "Node 1"

echo ""
echo "=========================================="
echo "Dependencies installed on both nodes"
echo "=========================================="
echo ""

# Copy to Node 0 (client + setup-env.sh)
copy_to_node "$NODE0" "Node 0" "client"

# Copy to Node 1 (server, stream + setup-env.sh)
copy_to_node "$NODE1" "Node 1" "server" "stream"

echo ""
echo "=== Compiling STREAM on Node 1 ==="
echo ""
ssh -i "$SSH_KEY" "${USER}@${NODE1}" bash << ENDSSH
    set -e  # Exit on error
    
    echo "Cleaning and compiling STREAM..."
    cd ${REMOTE_DIR}/stream
    make clean 2>/dev/null || true
    make
    
    if [ \$? -eq 0 ]; then
        echo ""
        echo "✓ STREAM compiled successfully!"
    else
        echo ""
        echo "✗ STREAM compilation failed!"
        exit 1
    fi
ENDSSH

if [ $? -eq 0 ]; then
    echo "Node 1 STREAM compilation completed"
else
    echo "Node 1 STREAM compilation failed"
    exit 1
fi

echo ""
echo "=========================================="
echo "All files copied successfully!"
echo "=========================================="
echo ""
echo "Summary:"
echo "  Node 0: ${REMOTE_DIR}/client/ + ${REMOTE_DIR}/setup-env.sh"
echo "  Node 1: ${REMOTE_DIR}/server/ + ${REMOTE_DIR}/stream/ + ${REMOTE_DIR}/setup-env.sh"
echo ""
echo "Next steps:"
echo "  1. SSH into each node and run setup-env.sh if needed"
echo "  2. Compile/build any necessary components"
echo "  3. Run your experiments"