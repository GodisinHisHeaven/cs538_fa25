Here I list all the deviations our experiments have compared with the original experiments in https://github.com/host-architecture/understanding-the-host-network/blob/master/sigcomm24/tcp.md.

- We do not use jumbo packets (9K MTU). Instead we opted to  use 4K MTU due to limitations in the Cloudlab network.

- We do not disable DDIO due to a lack of sources on how to do that for IceLake CPUs.

- We collected data with tso and gro turned on and off (which can be found in cloud-lab-results/iperf3_traces/11-17-2025/3-setup-env/ and cloud-lab-results/iperf3_traces/11-17-2025/4-disable-tcp-opt/, respectively)

- Hardware prefetching disabled for both client and server. The paper does not make it clear whether hardware prefetching is disabled on both.

<!-- Failed to stop irqbalance.service: Unit irqbalance.service not  in setup-envir.sh associated with 
```
#Enable aRFS, TSO, GRO for the interface
if [ "$opt" = 1 ]
then
    cd $home/Understanding-network-stack-overheads-SIGCOMM-2021/
    echo "Enabling TCP optimizations (TSO, GRO, aRFS)..."
    sudo python network_setup.py $intf --arfs --mtu $mtu --sock-size --tso --gro
    cd -
fi
```
So maybe not all optimizations enabled 
Hardware prefetching is enabled for some reason but not enabled in paper? -->