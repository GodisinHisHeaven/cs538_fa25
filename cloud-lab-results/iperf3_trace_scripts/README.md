To run the experiment follow these steps.

1. Request a 2 node setup from Cloudlab from cluster sm220u.
1. Configure `experiment_config.sh` with the necessary credentials.
1. Run `setup-node.sh`
1. SSH to both nodes and run `chmod +x disable-scaling.sh && bash disable-scaling.sh`
1. SSH to both nodes, modify the values in `setup-env.sh` with the correct intf and
addr, and run `chmod +x setup-env.sh && bash -c "sudo ./setup-env.sh"`.
1. On your main computer run `start-experiment.sh`. This will generate the pcap files and logs with the server loaded with the stream workload. To collect the baseline, you would need to manually run `start-server.sh` and `start_clients_multi.sh`. All artifacts will be generated on the corresponding Cloudlab nodes.

Note you can change the bash files in client and server folders to tweak the Experiment Parameters but just remember that the bash files on the corresponding Cloudlab nodes should be the most up-to-date version you want to run the experiment with. Changing the files on your main computer will not reflect these changes.

