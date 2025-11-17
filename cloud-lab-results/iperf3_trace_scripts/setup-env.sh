#!/bin/bash
# collect ping data for propogation delay
# Disable frequency scaling and turbo boost for deterministic measurements
# sudo ./disable-scaling.sh
# Parameter values used in original experiment (https://github.com/host-architecture/understanding-the-host-network/blob/master/sigcomm24/tcp.md)

#Modify these values
intf="ens2f0np0"
addr=
mtu=4000

# home='/users/edwinji2'
# opt=1
# ddio=0
# hwpref=1
# rdma=0
# pfc=0
# buf=1
# ecn=1

# RDMA check (skipped since rdma=0)
# No MTU adjustment needed
# set -x
# Setup the interface
echo "Setting up the interface...$intf"
ifconfig $intf up
ifconfig $intf $addr
ifconfig $intf mtu $mtu

# Socket buffer setup (using default buf=1)
echo "Setting up the socket buffer size to be 1MB"
echo 0 > /proc/sys/net/ipv4/tcp_moderate_rcvbuf 
echo "2000000 2000000 2000000" > /proc/sys/net/ipv4/tcp_rmem 
echo "1000000 1000000 1000000" > /proc/sys/net/ipv4/tcp_wmem

# Enable ECN (using default ecn=1)
echo "Enabling ECN support..."
echo 1 > /proc/sys/net/ipv4/tcp_ecn

# Enable TCP optimizations (opt=1)
echo "Enabling TCP optimizations (TSO, GRO, aRFS)..."
# 1. Enable offloads
ethtool -K $intf tso on gro on

# 2. Stop IRQ balancing service
service irqbalance stop

# 3. Enable RPS (Receive Packet Steering)
echo 32768 > /proc/sys/net/core/rps_sock_flow_entries
for f in /sys/class/net/$intf/queues/rx-*/rps_flow_cnt; do echo 32768 > $f; done

# 4. Enable ntuple (flow steering)
ethtool -K $intf ntuple on

# 5. Set IRQ affinity
set_irq_affinity.sh $intf 2> /dev/null > /dev/null

# 6. Clear all existing flow steering rules
for ((i=0; i<=1023; i++)); do ethtool -U $intf delete "$i" 2>/dev/null >/dev/null; done

# 7. Set MTU
ifconfig $intf mtu $mtu

# 8. Increase socket buffer limits
sysctl -w net.core.wmem_max=12582912 && sysctl -w net.core.rmem_max=12582912

# # Disable DDIO (ddio=0)
# cd /users/edwinji2/ddio-bench/
# echo "Disabling DDIO..."
# ./change-ddio-off
# cd -

# Disable hardware prefetching (hwpref=1)
echo "Disabling hardware prefetching..."
modprobe msr
wrmsr -a 0x1a4 15

# Disable PFC (pfc=0)
echo "Disabling PFC..."
sudo mlnx_qos -i $intf --pfc 0,0,0,0,0,0,0,0