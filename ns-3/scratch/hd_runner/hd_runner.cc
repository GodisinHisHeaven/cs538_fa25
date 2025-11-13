/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * CS538 Host Delay Experiment Runner - iperf3 Version
 *
 * A deterministic experiment harness using iperf3 over DCTCP
 * Features:
 * - Deterministic Host0 → Switch → Host1 topology
 * - Ping-pong and RPC workloads
 * - No-op delay hooks (DelayEgress/DelayIngress) for future model integration
 * - Per-request latency logging (JSONL)
 * - Optional event timeline logging
 * - Summary statistics (p50/p95/p99)
 * - PCAP capture support (client-side only)
 */

#include "delay_hooks.h"
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

NS_LOG_COMPONENT_DEFINE("Iperf3DctcpRunner");

// ============================================================================
// Global Configuration and State
// ============================================================================

struct RunConfig
{
    // Network parameters
    std::string linkRate = "25Gbps";
    std::string linkDelay = "58.65us"; // 118.7us|58.65us
    uint32_t mtu = 4000;  // 4K MTU for DCTCP
    std::string tcpVariant = "TcpDctcp";

    // iperf3 parameters
    uint32_t duration = 10;  // Test duration in seconds
    std::string dataRate = "10Gbps";  // Target data rate for iperf3

    // Hook parameters
    bool enableEgressHook = false;
    bool enableIngressHook = false;
    std::string hookConfigPath = "";

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

static FlowStats g_flowStats;

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
    hash ^= std::hash<std::string>{}(g_config.dataRate);
    hash ^= g_config.duration * 31;

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
    ofs << "  \"dataRate\": \"" << g_config.dataRate << "\",\n";
    ofs << "  \"enableEgressHook\": " << (g_config.enableEgressHook ? "true" : "false") << ",\n";
    ofs << "  \"enableIngressHook\": " << (g_config.enableIngressHook ? "true" : "false") << ",\n";
    ofs << "  \"hookConfigPath\": \"" << g_config.hookConfigPath << "\",\n";
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

    double duration = g_flowStats.endTime - g_flowStats.startTime;
    double throughputMbps = (g_flowStats.rxBytes * 8.0) / (duration * 1e6);

    ofs << "CS538 iperf3 DCTCP Experiment - Summary\n";
    ofs << "========================================\n\n";

    ofs << "Run ID: " << g_config.runId << "\n\n";

    ofs << "Configuration:\n";
    ofs << "--------------\n";
    ofs << "TCP variant:     " << g_config.tcpVariant << "\n";
    ofs << "Duration:        " << g_config.duration << " seconds\n";
    ofs << "Target rate:     " << g_config.dataRate << "\n";
    ofs << "Link rate:       " << g_config.linkRate << "\n";
    ofs << "Link delay:      " << g_config.linkDelay << "\n";
    ofs << "MTU:             " << g_config.mtu << " bytes\n";
    ofs << "Egress hook:     " << (g_config.enableEgressHook ? "enabled" : "disabled") << "\n";
    ofs << "Ingress hook:    " << (g_config.enableIngressHook ? "enabled" : "disabled") << "\n";
    ofs << "PCAP enabled:    " << (g_config.enablePcap ? "yes (client only)" : "no") << "\n";
    ofs << "Seed:            " << g_config.seed << "\n\n";

    ofs << "Results:\n";
    ofs << "--------\n";
    ofs << "Actual duration: " << std::fixed << std::setprecision(3) << duration << " seconds\n";
    ofs << "TX bytes:        " << g_flowStats.txBytes << "\n";
    ofs << "RX bytes:        " << g_flowStats.rxBytes << "\n";
    ofs << "TX packets:      " << g_flowStats.txPackets << "\n";
    ofs << "RX packets:      " << g_flowStats.rxPackets << "\n";
    ofs << "Throughput:      " << std::fixed << std::setprecision(2) << throughputMbps << " Mbps\n";
    ofs << "Throughput:      " << std::fixed << std::setprecision(2) << (throughputMbps / 1000.0) << " Gbps\n";

    if (g_flowStats.txPackets > 0)
    {
        double lossRate = 100.0 * (1.0 - (double)g_flowStats.rxPackets / g_flowStats.txPackets);
        ofs << "Loss rate:       " << std::fixed << std::setprecision(4) << lossRate << " %\n";
    }

    ofs.close();
    NS_LOG_INFO("Wrote summary to summary.txt");

    // Also print to console
    std::cout << "\n=== Summary ===\n";
    std::cout << "Duration: " << std::fixed << std::setprecision(3) << duration << " seconds\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << throughputMbps << " Mbps";
    std::cout << " (" << (throughputMbps / 1000.0) << " Gbps)\n";
    std::cout << "TX/RX bytes: " << g_flowStats.txBytes << " / " << g_flowStats.rxBytes << "\n";
    std::cout << "TX/RX packets: " << g_flowStats.txPackets << " / " << g_flowStats.rxPackets << "\n";
    
    if (g_config.enablePcap)
    {
        std::cout << "\nPCAP file written to: " << g_config.fullOutDir << "\n";
        std::cout << "  - iperf3_dctcp-client.pcap (client interface)\n";
    }
}

// ============================================================================
// Flow Monitor Callbacks
// ============================================================================

void
TxTrace(Ptr<const Packet> packet)
{
    g_flowStats.txBytes += packet->GetSize();
    g_flowStats.txPackets++;
}

void
RxTrace(Ptr<const Packet> packet, const Address& address)
{
    g_flowStats.rxBytes += packet->GetSize();
    g_flowStats.rxPackets++;
}

// ============================================================================
// Topology Setup
// ============================================================================

void
SetupTopology(NodeContainer& hosts, Ipv4InterfaceContainer& interfaces)
{
    NS_LOG_INFO("Setting up Host0 → Switch → Host1 topology for iperf3");

    // Create 3 nodes: Host0 (client), Switch, Host1 (server)
    NodeContainer allNodes;
    allNodes.Create(3);
    Ptr<Node> host0 = allNodes.Get(0);
    Ptr<Node> switchNode = allNodes.Get(1);
    Ptr<Node> host1 = allNodes.Get(2);

    hosts.Add(host0);  // Host0 - iperf3 client
    hosts.Add(host1);  // Host1 - iperf3 server

    // Configure point-to-point links
    PointToPointHelper p2p;
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

    // Install Internet stack with TCP DCTCP configuration
    InternetStackHelper stack;

    // Configure TCP DCTCP
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::" + g_config.tcpVariant));
    
    // DCTCP specific settings
    Config::SetDefault("ns3::TcpSocketBase::UseEcn", StringValue("On"));
    
    // Configure RED queue disc for ECN marking
    Config::SetDefault("ns3::RedQueueDisc::UseEcn", BooleanValue(true));
    Config::SetDefault("ns3::RedQueueDisc::UseHardDrop", BooleanValue(false));
    Config::SetDefault("ns3::RedQueueDisc::MeanPktSize", UintegerValue(g_config.mtu));
    Config::SetDefault("ns3::RedQueueDisc::QW", DoubleValue(1.0));
    Config::SetDefault("ns3::RedQueueDisc::MinTh", DoubleValue(20));
    Config::SetDefault("ns3::RedQueueDisc::MaxTh", DoubleValue(60));
    
    // TCP buffer sizes - 1 MB (fixed, no auto-tuning)
    // Disable auto-tuning by setting min/max to same value
    Config::SetDefault("ns3::TcpSocket::SndBufSize", UintegerValue(1048576)); // 1MB
    Config::SetDefault("ns3::TcpSocket::RcvBufSize", UintegerValue(1048576)); // 1MB
    Config::SetDefault("ns3::TcpSocketState::EnablePacing", BooleanValue(false)); // Disable pacing
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(g_config.mtu - 52)); // MTU - headers
    Config::SetDefault("ns3::TcpSocketBase::WindowScaling", BooleanValue(true)); // Enable window scaling
    
    // Set min/max to same value to disable auto-tuning (if supported by variant)
    Config::SetDefault("ns3::TcpSocketBase::MinRto", TimeValue(Seconds(0.2))); // Min RTO
    Config::SetDefault("ns3::TcpSocketBase::Timestamp", BooleanValue(true)); // Enable timestamps for better RTT
    
    // Initial congestion window
    Config::SetDefault("ns3::TcpSocket::InitialCwnd", UintegerValue(10)); // Start with 10 segments

    stack.Install(allNodes);

    // Install RED queue disc on switch interfaces for DCTCP ECN marking
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::RedQueueDisc");
    tch.Install(devices0);
    tch.Install(devices1);

    // Assign IP addresses
    Ipv4AddressHelper address;

    // Subnet for Host0 ↔ Switch
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces0 = address.Assign(devices0);

    // Subnet for Switch ↔ Host1
    address.SetBase("10.1.2.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces1 = address.Assign(devices1);

    // Enable global routing
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Store host interfaces
    interfaces.Add(interfaces0.Get(0));  // Host0: 10.1.1.1
    interfaces.Add(interfaces1.Get(1));  // Host1: 10.1.2.2

    NS_LOG_INFO("Topology setup complete");
    NS_LOG_INFO("  Host0 (client): " << interfaces.GetAddress(0));
    NS_LOG_INFO("  Switch: 10.1.1.2 / 10.1.2.1");
    NS_LOG_INFO("  Host1 (server): " << interfaces.GetAddress(1));
}

// ============================================================================
// PCAP Capture Setup (Client-side only
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
    std::string pcapPrefix = g_config.fullOutDir + "/iperf3_dctcp-client";


    // Capture only on host0 (client) - device 0
    p2p.EnablePcap(pcapPrefix, hosts.Get(0)->GetDevice(0), false);

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
    cmd.AddValue("tcpVariant", "TCP variant (TcpDctcp recommended)", g_config.tcpVariant);

    // iperf3 parameters
    cmd.AddValue("duration", "Test duration in seconds", g_config.duration);
    cmd.AddValue("dataRate", "Target data rate", g_config.dataRate);

    // Hook parameters
    cmd.AddValue("enableEgressHook", "Enable egress hook", g_config.enableEgressHook);
    cmd.AddValue("enableIngressHook", "Enable ingress hook", g_config.enableIngressHook);
    cmd.AddValue("hookConfigPath", "Path to hook config file", g_config.hookConfigPath);

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

    NS_LOG_INFO("CS538 iperf3 DCTCP Experiment Runner");
    NS_LOG_INFO("Run ID: " << g_config.runId);
    NS_LOG_INFO("Output: " << g_config.fullOutDir);

    // Initialize delay hooks
    DelayHooks::Initialize(g_config.hookConfigPath,
                          g_config.enableEgressHook,
                          g_config.enableIngressHook,
                          g_config.seed);

    // Setup topology
    NodeContainer hosts;
    Ipv4InterfaceContainer interfaces;
    SetupTopology(hosts, interfaces);

    // Enable PCAP capture on client only

    EnablePcapCapture(hosts);

    // Setup iperf3-like bulk send application
    uint16_t port = 5201;  // Standard iperf3 port

    // Server (sink) on Host1
    PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sinkHelper.Install(hosts.Get(1));
    sinkApp.Start(Seconds(0.0));
    sinkApp.Stop(Seconds(g_config.duration + 5.0));

    // Client (bulk sender) on Host0
    // Note: BulkSendApplication doesn't support rate limiting
    // For rate-limited TCP, would need OnOffApplication with DataRate attribute
    // Current implementation sends as fast as TCP allows (like iperf3 without -b)
    BulkSendHelper bulkSendHelper("ns3::TcpSocketFactory",
                                  InetSocketAddress(interfaces.GetAddress(1), port));
    bulkSendHelper.SetAttribute("MaxBytes", UintegerValue(0)); // Unlimited
    bulkSendHelper.SetAttribute("SendSize", UintegerValue(g_config.mtu - 52)); // Segment size
    
    ApplicationContainer clientApp = bulkSendHelper.Install(hosts.Get(0));
    clientApp.Start(Seconds(0.5));
    clientApp.Stop(Seconds(g_config.duration + 0.5));

    // Record flow start time
    g_flowStats.startTime = 0.5;
    g_flowStats.endTime = g_config.duration + 0.5;

    // Connect trace sources for statistics
    // Note: Use the application pointers directly instead of Config paths
    Ptr<BulkSendApplication> bulkSend = DynamicCast<BulkSendApplication>(clientApp.Get(0));
    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApp.Get(0));
    
    if (bulkSend)
    {
        bulkSend->TraceConnectWithoutContext("Tx", MakeCallback(&TxTrace));
    }
    
    if (sink)
    {
        sink->TraceConnectWithoutContext("Rx", MakeCallback(&RxTrace));
    }

    NS_LOG_INFO("Starting iperf3 simulation");
    NS_LOG_INFO("  Duration: " << g_config.duration << " seconds");
    NS_LOG_INFO("  Target rate: " << g_config.dataRate);
    NS_LOG_INFO("  MTU: " << g_config.mtu << " bytes");
    NS_LOG_INFO("  TCP variant: " << g_config.tcpVariant);
    if (g_config.enablePcap)
    {
        NS_LOG_INFO("  PCAP: enabled (client-side only)");

    }

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
// ================================================================================
// TCP RTT Latency Analysis
// ================================================================================

// Analyzing: iperf3_dctcp-client-0-0.pcap
// --------------------------------------------------------------------------------
//   Sample count: 2335887
//   Min RTT:      0.234 ms
//   Mean RTT:     0.238 ms
//   Median (p50): 0.237 ms
//   p90:          0.238 ms
//   p95:          0.238 ms
//   p99:          0.238 ms
//   p99.9:        0.482 ms
//   Max RTT:      201.913 ms
//   Std Dev:      0.186 ms