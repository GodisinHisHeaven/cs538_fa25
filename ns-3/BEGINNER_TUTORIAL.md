# NS-3 Beginner Tutorial: Your First Network Simulation

## Overview

I've created a comprehensive tutorial simulation (`my-first-network.cc`) that demonstrates the fundamental concepts of NS-3. This guide will help you understand how network simulations work in NS-3.

## What the Simulation Does

The simulation creates a **star topology network** with:
- 1 central server node (Node 0)
- 3 client nodes (Nodes 1, 2, 3)
- Point-to-point links connecting each client to the server
- UDP echo applications for testing communication

```
     Node-1
       |
       | 5Mbps, 2ms
       |
    Node-0 (Server)
      / \
5Mbps/   \5Mbps
 2ms/     \2ms
   /       \
Node-2   Node-3
```

## Key NS-3 Concepts Explained

### 1. **Nodes**
- Basic computing devices in the simulation
- Can represent computers, routers, switches, etc.
- Created using `NodeContainer`

### 2. **Channels and NetDevices**
- **Channel**: The medium connecting nodes (like ethernet cable, WiFi)
- **NetDevice**: Network interface card (NIC) on each node
- We use `PointToPointHelper` to create point-to-point links

### 3. **Protocol Stack**
- Internet stack (TCP/IP) installed on nodes
- Enables IP addressing and routing
- Installed using `InternetStackHelper`

### 4. **IP Addresses**
- Each link gets its own subnet (10.1.1.0/24, 10.1.2.0/24, etc.)
- Assigned using `Ipv4AddressHelper`

### 5. **Applications**
- Generate network traffic
- **UdpEchoServer**: Listens on port 9, echoes back received packets
- **UdpEchoClient**: Sends packets to server, receives echoes

### 6. **Simulation Time**
- NS-3 uses discrete event simulation
- Events scheduled at specific times (e.g., "send packet at 2.5 seconds")

## Running the Simulation

### Basic Run
```bash
./ns3 run my-first-network
```

### With Different Parameters
```bash
# Change number of nodes
./ns3 run "my-first-network --nNodes=6"

# Change simulation time
./ns3 run "my-first-network --simTime=20"

# Disable verbose output
./ns3 run "my-first-network --verbose=false"

# Disable pcap tracing
./ns3 run "my-first-network --tracing=false"
```

## Understanding the Output

When you run the simulation, you'll see:

1. **Setup Messages**: Creating nodes, links, installing protocols
2. **Packet Transmission**:
   - "client sent X bytes" - Client sends a packet
   - "server received X bytes" - Server receives the packet
   - "server sent X bytes" - Server echoes back
   - "client received X bytes" - Client receives the echo

3. **Timing Information**:
   - `+2.5s` - Simulation time when event occurs
   - Notice the ~3.7ms round-trip time (due to 2ms link delay each way)

## Output Files Generated

### 1. PCAP Files (`my-first-network-*.pcap`)
- Packet capture files for each network device
- Can be opened with Wireshark:
```bash
wireshark my-first-network-0-0.pcap
```

### 2. Animation File (`my-first-network.xml`)
- For visualization with NetAnim
- Download NetAnim from: https://www.nsnam.org/wiki/NetAnim
- Open the XML file in NetAnim to see animated packet flow

## Modifying the Simulation

### Change Network Topology
Edit the loop in Step 3 to create different topologies:
- **Linear**: Connect nodes in a chain
- **Ring**: Connect nodes in a circle
- **Mesh**: Connect every node to every other node

### Change Link Properties
```cpp
pointToPoint.SetDeviceAttribute ("DataRate", StringValue ("10Mbps"));
pointToPoint.SetChannelAttribute ("Delay", StringValue ("5ms"));
```

### Add Different Applications
Replace UDP echo with:
- TCP bulk transfer
- HTTP traffic
- Video streaming

### Add Packet Loss
```cpp
Ptr<RateErrorModel> em = CreateObject<RateErrorModel> ();
em->SetAttribute ("ErrorRate", DoubleValue (0.01)); // 1% packet loss
devices[i].Get(1)->SetAttribute ("ReceiveErrorModel", PointerValue (em));
```

## Learning Path

1. **Start Here**: Run and understand `my-first-network.cc`
2. **Next Steps**:
   - Modify parameters and observe changes
   - Try different topologies
   - Add more nodes
3. **Advanced**:
   - Add WiFi instead of point-to-point
   - Implement routing protocols
   - Add mobility models
   - Measure performance metrics

## Common NS-3 Patterns

### Creating Components
```cpp
NodeContainer nodes;
nodes.Create(n);
```

### Installing Protocols
```cpp
InternetStackHelper stack;
stack.Install(nodes);
```

### Setting Attributes
```cpp
app.SetAttribute("MaxPackets", UintegerValue(10));
```

### Scheduling Events
```cpp
Simulator::Schedule(Seconds(2.0), &Function, args);
```

## Troubleshooting

### Build Errors
```bash
./ns3 clean
./ns3 build
```

### Runtime Errors
- Check IP address configuration
- Verify application start/stop times
- Enable logging for debugging

### Performance Issues
- Reduce simulation time
- Decrease number of nodes
- Disable tracing/animation

## Next Tutorials to Explore

1. **first.cc** - Simplest possible simulation
2. **second.cc** - Adds logging
3. **third.cc** - Adds WiFi
4. **fourth.cc** - Adds mobility
5. **fifth.cc** - Adds routing

## Useful Commands

```bash
# List all examples
ls examples/tutorial/

# Run with logging
NS_LOG=MyFirstNetwork=level_all ./ns3 run my-first-network

# Run with GDB debugger
./ns3 run my-first-network --gdb

# Generate Doxygen documentation
./ns3 docs doxygen
```

## Resources

- **Official Tutorial**: https://www.nsnam.org/docs/tutorial/html/
- **API Documentation**: https://www.nsnam.org/docs/release/3.42/doxygen/
- **Examples**: Check `examples/` directory for more code
- **Models**: Check `src/` directory for available modules

## Tips for Beginners

1. **Start Small**: Begin with simple topologies and add complexity gradually
2. **Use Logging**: Enable logging to understand what's happening
3. **Visualize**: Use NetAnim to see your network in action
4. **Read Examples**: The `examples/` directory is your best friend
5. **Experiment**: Change parameters and see what happens
6. **Ask Questions**: NS-3 has an active mailing list and forum

Happy Simulating! 🚀