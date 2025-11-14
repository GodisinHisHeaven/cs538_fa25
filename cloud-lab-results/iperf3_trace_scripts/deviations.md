No jumbo packets only 4000 size packets (Not 9K MTU)
Failed to stop irqbalance.service: Unit irqbalance.service not  in setup-envir.sh associated with 
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

Hardware prefetching is enabled for some reason but not enabled in paper? Experiment follows command with hardware prefetching

DDIO should be disabled (not in new experiment)