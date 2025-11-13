sudo apt update
git clone https://github.com/host-architecture/understanding-the-host-network.git
cd ~/understanding-the-host-network/stream
make clean 2>/dev/null || true make ./stream ReadWrite64 10
sudo apt-get install msr-tools 
sudo apt-get install libpci-dev
cd ~
git clone https://github.com/aliireza/ddio-bench.git
git clone https://github.com/Terabit-Ethernet/Understanding-network-stack-overheads-SIGCOMM-2021.git
git clone https://github.com/Terabit-Ethernet/hostCC.git
cd ddio-bench

vi change-ddio.c # Change the bus
gcc change-ddio.c -o change-ddio -lpci sudo ./change-ddio

mkdir /tmp/mlnx-tools
cd /tmp/mlnx-tools
wget https://github.com/Mellanox/mlnx-tools/archive/refs/tags/v24.10.1.tar.gz
tar -xvf v24.10.1.tar.gz mlnx-tools-24.10.1/
cd /users/edwinji2/mlnx-tools-24.10.1
sudo make install
sudo mlnx_qos -i ens2f0np0 --pfc 0,0,0,0,0,0,0,0
cd /users/edwinji2/hostCC/utils
chmod +x ./setup-envir.sh

sudo ./setup-envir.sh -H /users/edwinji2 -i ens2f0np0 -a 10.10.1.1 -m 4000 -o 1 -d 0 -f 1 -r 0 -p 0
sudo ./setup-envir.sh -H /users/edwinji2 -i ens2f0np0 -a 10.10.1.2 -m 4000 -o 1 -d 0 -f 1 -r 0 -p 0
cd /users/edwinji2/hostCC/utils/tcp
chmod +x ./run-netapp-tput.sh
sudo ./run-netapp-tput.sh -m server -o test -S 4 -c 0,4,8,12