/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * CS538 Host Delay Experiment Runner
 *
 * A deterministic experiment harness using iperf3
 * - Deterministic Host0 → Switch → Host1 topology
 * - 4 parallel iperf3 flows
 * - Delay hooks (DelayEgress/DelayIngress)
 * - PCAP capture support (client-side only)
 */

#include "delay_hooks.h"
#include "point-to-point-helper-custom.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("Iperf3Runner");
// ============================================================================
// Global Configuration and State
// ============================================================================

struct RunConfig
{
    // Network parameters
    std::string linkRate = "100Gbps";  // 100 Gbps NICs
    std::string linkDelay = "26us"; // 0.105ms / 4 - 1
    uint32_t mtu = 4000;  // 4K MTU
    std::string tcpVariant = "TcpDctcp"; // Default to DCTCP

    // iperf3 parameters
    uint32_t duration = 1;  // Test duration in seconds
    uint32_t numFlows = 4;   // Number of parallel flows
    uint64_t maxBytes = 0;   // Max bytes per flow (0 = unlimited)

    // Hook parameters
    bool enableEgressHook = false;
    bool enableIngressHook = false;
    std::string delayModel = "CacheMiss";
    std::string hookConfig = ""; // Config depends on delayModel used, look in delay_hooks.cc for details

    // Simulation parameters
    uint32_t seed = 1;
    std::string runId = "auto";
    std::string outDir = "out/sim";

    // PCAP parameters
    bool enablePcap = false;

    // Derived
    std::string fullOutDir;
};

static RunConfig g_config;

// Flow statistics tracking
struct FlowStats
{
    uint64_t txBytes = 0;
    uint64_t rxBytes = 0;
    uint64_t txPackets = 0;
    uint64_t rxPackets = 0;
    double startTime = 0.0;
    double endTime = 0.0;
};

static std::vector<FlowStats> g_flowStats;

// Delay hook statistics tracking
struct HookStats
{
    uint64_t totalPackets = 0;
    uint64_t totalDelayNs = 0;
    uint64_t maxDelayNs = 0;
};

static std::map<uint32_t, HookStats> g_egressStats;
static std::map<uint32_t, HookStats> g_ingressStats;

// ============================================================================
// Utility Functions
// ============================================================================

std::string
GenerateRunId()
{
    std::time_t now = std::time(nullptr);
    std::tm* ltm = std::localtime(&now);

    std::ostringstream oss;
    oss << std::put_time(ltm, "%Y%m%d-%H%M%S");

    // Add short hash based on config
    uint32_t hash = g_config.seed;
    hash ^= g_config.duration * 31;
    hash ^= g_config.numFlows * 17;

    oss << "-" << std::hex << std::setw(6) << std::setfill('0') << (hash & 0xFFFFFF);

    return oss.str();
}

void
CreateDirectories(const std::string& path)
{
    std::string cmd = "mkdir -p " + path;
    int ret = system(cmd.c_str());
    if (ret != 0)
    {
        NS_FATAL_ERROR("Failed to create directory: " << path);
    }
}

// ============================================================================
// Logging Functions
// ============================================================================

void
WriteConfigLog()
{
    std::string path = g_config.fullOutDir + "/config.json";
    std::ofstream ofs(path);

    if (!ofs.is_open())
    {
        NS_LOG_ERROR("Failed to open config.json for writing");
        return;
    }

    ofs << "{\n";
    ofs << "  \"linkRate\": \"" << g_config.linkRate << "\",\n";
    ofs << "  \"linkDelay\": \"" << g_config.linkDelay << "\",\n";
    ofs << "  \"mtu\": " << g_config.mtu << ",\n";
    ofs << "  \"tcpVariant\": \"" << g_config.tcpVariant << "\",\n";
    ofs << "  \"duration\": " << g_config.duration << ",\n";
    ofs << "  \"numFlows\": " << g_config.numFlows << ",\n";
    ofs << "  \"maxBytesPerFlow\": " << g_config.maxBytes << ",\n";
    ofs << "  \"enableEgressHook\": " << (g_config.enableEgressHook ? "true" : "false") << ",\n";
    ofs << "  \"enableIngressHook\": " << (g_config.enableIngressHook ? "true" : "false") << ",\n";
    ofs << "  \"delayModel\": \"" << g_config.delayModel << "\",\n";
    ofs << "  \"hookConfig\": \"" << g_config.hookConfig << "\",\n";
    ofs << "  \"enablePcap\": " << (g_config.enablePcap ? "true" : "false") << ",\n";
    ofs << "  \"seed\": " << g_config.seed << ",\n";
    ofs << "  \"runId\": \"" << g_config.runId << "\"\n";
    ofs << "}\n";

    ofs.close();
    NS_LOG_INFO("Wrote config to config.json");
}

void
WriteSummary()
{
    std::string path = g_config.fullOutDir + "/summary.txt";
    std::ofstream ofs(path);

    if (!ofs.is_open())
    {
        NS_LOG_ERROR("Failed to open summary.txt for writing");
        return;
    }

    ofs << "CS538 iperf3 Experiment - Summary\n";
    ofs << "===================================================\n\n";

    ofs << "Run ID: " << g_config.runId << "\n\n";

    ofs << "Configuration:\n";
    ofs << "--------------\n";
    ofs << "TCP variant:     " << g_config.tcpVariant << "\n";
    ofs << "Duration:        " << g_config.duration << " seconds\n";
    ofs << "Number of flows: " << g_config.numFlows << "\n";
    ofs << "Max bytes/flow:  " << (g_config.maxBytes == 0 ? "unlimited" : std::to_string(g_config.maxBytes)) << "\n";
    ofs << "Link rate:       " << g_config.linkRate << "\n";
    ofs << "Link delay:      " << g_config.linkDelay << "\n";
    ofs << "MTU:             " << g_config.mtu << " bytes\n";
    ofs << "Socket buffers:  1 MB (fixed)\n";
    ofs << "Egress hook:     " << (g_config.enableEgressHook ? "enabled" : "disabled") << "\n";
    ofs << "Ingress hook:    " << (g_config.enableIngressHook ? "enabled" : "disabled") << "\n";
    ofs << "PCAP enabled:    " << (g_config.enablePcap ? "yes (client only)" : "no") << "\n";
    ofs << "Seed:            " << g_config.seed << "\n\n";

    // Per-flow statistics
    uint64_t totalTxBytes = 0;
    uint64_t totalRxBytes = 0;
    uint64_t totalTxPackets = 0;
    uint64_t totalRxPackets = 0;

    ofs << "Per-Flow Results:\n";
    ofs << "-----------------\n";
    for (size_t i = 0; i < g_flowStats.size(); i++)
    {
        const FlowStats& fs = g_flowStats[i];
        double duration = fs.endTime - fs.startTime;
        double throughputMbps = (fs.rxBytes * 8.0) / (duration * 1e6);

        ofs << "\nFlow " << i << " (port " << (5201 + i) << "):\n";
        ofs << "  Duration:   " << std::fixed << std::setprecision(3) << duration << " seconds\n";
        ofs << "  TX bytes:   " << fs.txBytes << "\n";
        ofs << "  RX bytes:   " << fs.rxBytes << "\n";
        ofs << "  TX packets: " << fs.txPackets << "\n";
        ofs << "  RX packets: " << fs.rxPackets << "\n";
        ofs << "  Throughput: " << std::fixed << std::setprecision(2) << throughputMbps << " Mbps";
        ofs << " (" << (throughputMbps / 1000.0) << " Gbps)\n";

        if (fs.txPackets > 0)
        {
            double lossRate = 100.0 * (1.0 - (double)fs.rxPackets / fs.txPackets);
            ofs << "  Loss rate:  " << std::fixed << std::setprecision(4) << lossRate << " %\n";
        }

        totalTxBytes += fs.txBytes;
        totalRxBytes += fs.rxBytes;
        totalTxPackets += fs.txPackets;
        totalRxPackets += fs.rxPackets;
    }

    // Aggregate statistics
    double avgDuration = 0.0;
    for (const auto& fs : g_flowStats)
    {
        avgDuration += (fs.endTime - fs.startTime);
    }
    avgDuration /= g_flowStats.size();

    double totalThroughputMbps = (totalRxBytes * 8.0) / (avgDuration * 1e6);

    ofs << "\n\nAggregate Results:\n";
    ofs << "------------------\n";
    ofs << "Total TX bytes:      " << totalTxBytes << "\n";
    ofs << "Total RX bytes:      " << totalRxBytes << "\n";
    ofs << "Total TX packets:    " << totalTxPackets << "\n";
    ofs << "Total RX packets:    " << totalRxPackets << "\n";
    ofs << "Aggregate throughput: " << std::fixed << std::setprecision(2) << totalThroughputMbps << " Mbps";
    ofs << " (" << (totalThroughputMbps / 1000.0) << " Gbps)\n";

    if (totalTxPackets > 0)
    {
        double overallLossRate = 100.0 * (1.0 - (double)totalRxPackets / totalTxPackets);
        ofs << "Overall loss rate:    " << std::fixed << std::setprecision(4) << overallLossRate << " %\n";
    }

    // Delay hook statistics
    if (g_config.enableEgressHook || g_config.enableIngressHook)
    {
        ofs << "\n\nDelay Hook Statistics:\n";
        ofs << "----------------------\n";

        if (g_config.enableEgressHook)
        {
            ofs << "\nEgress Hook (MacTx - before NIC transmission):\n";
            for (const auto& entry : g_egressStats)
            {
                uint32_t nodeId = entry.first;
                const HookStats& stats = entry.second;
                if (stats.totalPackets > 0)
                {
                    double avgDelayUs = (double)stats.totalDelayNs / stats.totalPackets / 1000.0;
                    double maxDelayUs = (double)stats.maxDelayNs / 1000.0;
                    ofs << "  Node " << nodeId << ":\n";
                    ofs << "    Packets processed: " << stats.totalPackets << "\n";
                    ofs << "    Avg calculated delay: " << std::fixed << std::setprecision(3)
                        << avgDelayUs << " us\n";
                    ofs << "    Max calculated delay: " << std::fixed << std::setprecision(3)
                        << maxDelayUs << " us\n";
                }
            }
        }

        if (g_config.enableIngressHook)
        {
            ofs << "\nIngress Hook (MacRx - before L3 delivery):\n";
            for (const auto& entry : g_ingressStats)
            {
                uint32_t nodeId = entry.first;
                const HookStats& stats = entry.second;
                if (stats.totalPackets > 0)
                {
                    double avgDelayUs = (double)stats.totalDelayNs / stats.totalPackets / 1000.0;
                    double maxDelayUs = (double)stats.maxDelayNs / 1000.0;
                    ofs << "  Node " << nodeId << ":\n";
                    ofs << "    Packets processed: " << stats.totalPackets << "\n";
                    ofs << "    Avg calculated delay: " << std::fixed << std::setprecision(3)
                        << avgDelayUs << " us\n";
                    ofs << "    Max calculated delay: " << std::fixed << std::setprecision(3)
                        << maxDelayUs << " us\n";
                }
            }
        }

        ofs << "\nNOTE: Delay injection implementation using DelayedPointToPointChannel.\n";
        ofs << "Egress delays are injected at the channel level during packet transmission.\n";
        ofs << "Statistics above show the delays calculated and applied to each packet.\n";
    }

    ofs.close();
    NS_LOG_INFO("Wrote summary to summary.txt");

    // Also print to console
    std::cout << "\n=== Summary ===\n";
    std::cout << "Flows: " << g_config.numFlows << "\n";
    std::cout << "Aggregate throughput: " << std::fixed << std::setprecision(2) << totalThroughputMbps << " Mbps";
    std::cout << " (" << (totalThroughputMbps / 1000.0) << " Gbps)\n";
    std::cout << "Total TX/RX bytes: " << totalTxBytes << " / " << totalRxBytes << "\n";
    std::cout << "Total TX/RX packets: " << totalTxPackets << " / " << totalRxPackets << "\n";

    if (g_config.enablePcap)
    {
        std::cout << "\nPCAP file written to: " << g_config.fullOutDir << "\n";
        std::cout << "  - iperf3_" << g_config.tcpVariant << "-client.pcap (client interface)\n";
    }
}

// ============================================================================
// Delay Hook Callbacks
// ============================================================================

// Per-node sequence counters for hook tracking
static std::map<uint32_t, uint32_t> g_egressSeq;
static std::map<uint32_t, uint32_t> g_ingressSeq;

void
EgressTraceCallback(uint32_t nodeId, Ptr<const Packet> packet)
{
    if (!DelayHooks::IsEgressEnabled())
        return;

    uint32_t seq = g_egressSeq[nodeId]++;
    uint32_t bytes = packet->GetSize();
    Time delay = DelayHooks::DelayEgress(nodeId, bytes, seq);

    // Track statistics
    g_egressStats[nodeId].totalPackets++;
    g_egressStats[nodeId].totalDelayNs += delay.GetNanoSeconds();
    if (delay.GetNanoSeconds() > g_egressStats[nodeId].maxDelayNs)
    {
        g_egressStats[nodeId].maxDelayNs = delay.GetNanoSeconds();
    }

    // Log if delay is non-zero
    if (delay.GetNanoSeconds() > 0)
    {
        NS_LOG_DEBUG("Egress hook: node=" << nodeId << " seq=" << seq << " bytes=" << bytes
                                          << " delay=" << delay.GetMicroSeconds() << "us");
    }
}

void
IngressTraceCallback(uint32_t nodeId, Ptr<const Packet> packet)
{
    if (!DelayHooks::IsIngressEnabled())
        return;

    uint32_t seq = g_ingressSeq[nodeId]++;
    uint32_t bytes = packet->GetSize();
    Time delay = DelayHooks::DelayIngress(nodeId, bytes, seq);

    // Track statistics
    g_ingressStats[nodeId].totalPackets++;
    g_ingressStats[nodeId].totalDelayNs += delay.GetNanoSeconds();
    if (delay.GetNanoSeconds() > g_ingressStats[nodeId].maxDelayNs)
    {
        g_ingressStats[nodeId].maxDelayNs = delay.GetNanoSeconds();
    }

    // Log if delay is non-zero
    if (delay.GetNanoSeconds() > 0)
    {
        NS_LOG_DEBUG("Ingress hook: node=" << nodeId << " seq=" << seq << " bytes=" << bytes
                                           << " delay=" << delay.GetMicroSeconds() << "us");
    }
}

// ============================================================================
// Flow Monitor Callbacks
// ============================================================================

void
TxTrace(uint32_t flowId, Ptr<const Packet> packet)
{
    g_flowStats[flowId].txBytes += packet->GetSize();
    g_flowStats[flowId].txPackets++;
}

void
RxTrace(uint32_t flowId, Ptr<const Packet> packet, const Address& address)
{
    g_flowStats[flowId].rxBytes += packet->GetSize();
    g_flowStats[flowId].rxPackets++;
}

// ============================================================================
// Topology Setup
// ============================================================================

void
SetupTopology(NodeContainer& hosts, Ipv4InterfaceContainer& interfaces)
{
    NS_LOG_INFO("Setting up Host0 (server) - Switch - Host1 (client) topology for iperf3");
    NS_LOG_INFO("Using DelayedPointToPointChannel for delay injection");

    // Create 3 nodes: Host0 (server), Switch, Host1 (client)
    NodeContainer allNodes;
    allNodes.Create(3);
    Ptr<Node> host0 = allNodes.Get(0);
    Ptr<Node> switchNode = allNodes.Get(1);
    Ptr<Node> host1 = allNodes.Get(2);

    hosts.Add(host0);  // Host0 - iperf3 server
    hosts.Add(host1);  // Host1 - iperf3 client

    // Configure custom point-to-point links with delay injection
    PointToPointHelperCustom p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(g_config.linkRate));
    p2p.SetChannelAttribute("Delay", StringValue(g_config.linkDelay));
    p2p.SetDeviceAttribute("Mtu", UintegerValue(g_config.mtu));

    // Create link: Host0 ↔ Switch
    NodeContainer link0;
    link0.Add(host0);
    link0.Add(switchNode);
    NetDeviceContainer devices0 = p2p.Install(link0);

    // Create link: Switch ↔ Host1
    NodeContainer link1;
    link1.Add(switchNode);
    link1.Add(host1);
    NetDeviceContainer devices1 = p2p.Install(link1);

    // Install Internet stack with TCP configuration
    InternetStackHelper stack;

    // Configure TCP variant
    std::string transport_prot = std::string("ns3::") + g_config.tcpVariant;
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", TypeIdValue(TypeId::LookupByName(transport_prot)));

    // TCP buffer sizes - 1 MB (fixed, no auto-tuning)
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(1 << 20)); // 1MB
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(1 << 20)); // 1MB
    Config::SetDefault("ns3::TcpSocketState::EnablePacing", BooleanValue(false)); // Disable pacing
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(g_config.mtu - 52)); // MTU - headers
    Config::SetDefault("ns3::TcpSocketBase::WindowScaling", BooleanValue(true)); // Enable window scaling

    Config::SetDefault("ns3::TcpSocketBase::MinRto", TimeValue(Seconds(0.2))); // Min RTO
    Config::SetDefault("ns3::TcpSocketBase::Timestamp", BooleanValue(true)); // Enable timestamps

    // Initial congestion window
    Config::SetDefault("ns3::TcpSocket::InitialCwnd", UintegerValue(10));

    stack.Install(allNodes);

    // Assign IP addresses
    Ipv4AddressHelper address;

    // Subnet for Host0 ↔ Switch
    address.SetBase("10.10.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces0 = address.Assign(devices0);

    // Subnet for Switch ↔ Host1
    address.SetBase("10.10.2.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces1 = address.Assign(devices1);

    // Enable global routing
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Store host interfaces
    interfaces.Add(interfaces0.Get(0));  // Host0 (server): 10.10.1.1
    interfaces.Add(interfaces1.Get(1));  // Host1 (client): 10.10.2.2

    NS_LOG_INFO("Topology setup complete");
    NS_LOG_INFO("  Host0 (server): " << interfaces.GetAddress(0));
    NS_LOG_INFO("  Switch: 10.10.1.2 / 10.10.2.1");
    NS_LOG_INFO("  Host1 (client): " << interfaces.GetAddress(1));
}

// ============================================================================
// PCAP Capture Setup (Client-side only)
// ============================================================================

void
EnablePcapCapture(const NodeContainer& hosts)
{
    if (!g_config.enablePcap)
    {
        return;
    }

    NS_LOG_INFO("Enabling PCAP capture on client host");

    PointToPointHelper p2p;
    std::string pcapPrefix = g_config.fullOutDir + "/iperf3_" + g_config.tcpVariant + "-client.pcap";

    // Capture only on host1 (client) - device 0
    p2p.EnablePcap(pcapPrefix, hosts.Get(1)->GetDevice(0), false, true);

    NS_LOG_INFO("PCAP capture enabled on client interface");
    NS_LOG_INFO("  File will be written to: " << pcapPrefix << ".pcap");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char* argv[])
{
    // Parse command line arguments
    CommandLine cmd;

    // Network parameters
    cmd.AddValue("linkRate", "Link data rate", g_config.linkRate);
    cmd.AddValue("linkDelay", "Link propagation delay", g_config.linkDelay);
    cmd.AddValue("mtu", "MTU size", g_config.mtu);
    cmd.AddValue("tcpVariant", "TCP variant (TcpDctcp default)", g_config.tcpVariant);

    // iperf3 parameters
    cmd.AddValue("duration", "Test duration in seconds", g_config.duration);
    cmd.AddValue("numFlows", "Number of parallel flows", g_config.numFlows);
    cmd.AddValue("maxBytes", "Max bytes to transmit per flow (0=unlimited)", g_config.maxBytes);

    // Hook parameters
    cmd.AddValue("enableEgressHook", "Enable egress hook", g_config.enableEgressHook);
    cmd.AddValue("enableIngressHook", "Enable ingress hook", g_config.enableIngressHook);
    cmd.AddValue("delayModel", "DelayModel to use (Default, CacheMiss, UHN)", g_config.delayModel);
    cmd.AddValue("hookConfig", "Config for DelayModel", g_config.hookConfig);

    // Simulation parameters
    cmd.AddValue("seed", "Random seed", g_config.seed);
    cmd.AddValue("runId", "Run ID (auto or custom)", g_config.runId);
    cmd.AddValue("outDir", "Output directory", g_config.outDir);

    // PCAP parameters
    cmd.AddValue("enablePcap", "Enable PCAP packet capture (client only)", g_config.enablePcap);

    cmd.Parse(argc, argv);

    // Set RNG seed for determinism
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(g_config.seed);

    // Generate run ID if needed
    if (g_config.runId == "auto")
    {
        g_config.runId = GenerateRunId();
    }

    // Create output directory
    g_config.fullOutDir = g_config.outDir + "/" + g_config.runId;
    CreateDirectories(g_config.fullOutDir);

    NS_LOG_INFO("CS538 iperf3 Experiment Runner");
    NS_LOG_INFO("Run ID: " << g_config.runId);
    NS_LOG_INFO("Output: " << g_config.fullOutDir);

    // Initialize delay hooks
    DelayHooks::Initialize(g_config.delayModel,
                          g_config.hookConfig,
                          g_config.enableEgressHook,
                          g_config.enableIngressHook,
                          g_config.seed);

    // Setup topology
    NodeContainer hosts;
    Ipv4InterfaceContainer interfaces;
    SetupTopology(hosts, interfaces);

    // Enable PCAP capture on client only
    EnablePcapCapture(hosts);

    // Connect delay hook traces to NetDevices for statistics tracking
    // Note: Actual delay injection happens in DelayedPointToPointChannel
    if (g_config.enableEgressHook || g_config.enableIngressHook)
    {
        NS_LOG_INFO("Connecting delay hook traces for statistics tracking");
        NS_LOG_INFO("  (Actual egress delay injection happens in DelayedPointToPointChannel)");
        for (uint32_t i = 0; i < hosts.GetN(); i++)
        {
            Ptr<Node> node = hosts.Get(i);
            Ptr<NetDevice> device = node->GetDevice(0);

            if (g_config.enableEgressHook)
            {
                bool connected = device->TraceConnectWithoutContext(
                    "MacTx", MakeBoundCallback(&EgressTraceCallback, i));
                NS_LOG_INFO("  Node " << i << " egress stats trace: "
                                      << (connected ? "connected" : "FAILED"));
            }

            if (g_config.enableIngressHook)
            {
                bool connected = device->TraceConnectWithoutContext(
                    "MacRx", MakeBoundCallback(&IngressTraceCallback, i));
                NS_LOG_INFO("  Node " << i << " ingress stats trace: "
                                      << (connected ? "connected" : "FAILED"));
            }
        }
    }

    // Initialize flow statistics
    g_flowStats.resize(g_config.numFlows);

    // Setup multiple iperf3-like flows (starting from port 5201)
    uint16_t basePort = 5201;

    NS_LOG_INFO("Setting up " << g_config.numFlows << " parallel iperf3 flows");

    for (uint32_t i = 0; i < g_config.numFlows; i++)
    {
        uint16_t port = basePort + i;

        // Server (sink) on Host0 (10.10.1.1)
        PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                    InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer sinkApp = sinkHelper.Install(hosts.Get(0));
        sinkApp.Start(Seconds(0.0));
        sinkApp.Stop(Seconds(g_config.duration + 5.0));

        // Client (bulk sender) on Host1 (10.10.2.2) -> connects to server at 10.10.1.1
        BulkSendHelper bulkSendHelper("ns3::TcpSocketFactory",
                                      InetSocketAddress(interfaces.GetAddress(0), port));
        bulkSendHelper.SetAttribute("MaxBytes", UintegerValue(g_config.maxBytes)); // 0 = unlimited
        bulkSendHelper.SetAttribute("SendSize", UintegerValue(g_config.mtu - 52)); // Segment size

        ApplicationContainer clientApp = bulkSendHelper.Install(hosts.Get(1));
        clientApp.Start(Seconds(0.5));
        clientApp.Stop(Seconds(g_config.duration + 0.5));

        // Record flow timing
        g_flowStats[i].startTime = 0.5;
        g_flowStats[i].endTime = g_config.duration + 0.5;

        // Connect trace sources for per-flow statistics
        Ptr<BulkSendApplication> bulkSend = DynamicCast<BulkSendApplication>(clientApp.Get(0));
        Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApp.Get(0));

        if (bulkSend)
        {
            bulkSend->TraceConnectWithoutContext("Tx", MakeBoundCallback(&TxTrace, i));
        }

        if (sink)
        {
            sink->TraceConnectWithoutContext("Rx", MakeBoundCallback(&RxTrace, i));
        }

        NS_LOG_INFO("  Flow " << i << ": port " << port);
    }

    NS_LOG_INFO("Starting iperf3 simulation");
    NS_LOG_INFO("  Flows: " << g_config.numFlows);
    NS_LOG_INFO("  Duration: " << g_config.duration << " seconds");
    NS_LOG_INFO("  Max bytes/flow: " << (g_config.maxBytes == 0 ? "unlimited" : std::to_string(g_config.maxBytes)));
    NS_LOG_INFO("  MTU: " << g_config.mtu << " bytes (4K)");
    NS_LOG_INFO("  Link rate: " << g_config.linkRate);
    NS_LOG_INFO("  TCP variant: " << g_config.tcpVariant);
    if (g_config.enablePcap)
    {
        NS_LOG_INFO("  PCAP: enabled (client-side only)");
    }
    
    // Set Host0 (server) with high CPU contention
    NodeProperties props;
    props.cpuCoreContention = 6;

    DelayHooks::SetNodeProperties(hosts.Get(0)->GetId(), props);

    // Run simulation
    Simulator::Stop(Seconds(g_config.duration + 10.0));
    Simulator::Run();

    NS_LOG_INFO("Simulation complete");

    // Write outputs
    WriteConfigLog();
    WriteSummary();

    Simulator::Destroy();

    std::cout << "\nResults written to: " << g_config.fullOutDir << "\n";

    return 0;
}
