/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * CS538 Host Delay Experiment Runner
 *
 * A deterministic experiment harness for measuring host-delay effects
 * on network tail latency. Features:
 * - Deterministic Host0 → Switch → Host1 topology
 * - Ping-pong and RPC workloads
 * - No-op delay hooks (DelayEgress/DelayIngress) for future model integration
 * - Per-request latency logging (JSONL)
 * - Optional event timeline logging
 * - Summary statistics (p50/p95/p99)
 */

#include "delay_hooks.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("HdRunner");

// ============================================================================
// Global Configuration and State
// ============================================================================

struct RunConfig
{
    // Network parameters
    std::string linkRate = "10Gbps";
    std::string linkDelay = "50us";
    uint32_t mtu = 1500;
    std::string qdisc = "none";  // none or fq_codel

    // Workload parameters
    std::string workload = "pingpong";  // pingpong or rpc
    uint32_t nReq = 10000;
    uint32_t outstanding = 1;
    uint32_t reqBytes = 1024;
    uint32_t rspBytes = 1024;

    // Hook parameters
    bool enableEgressHook = true;
    bool enableIngressHook = true;
    std::string hookConfigPath = "";

    // Simulation parameters
    uint32_t seed = 1;
    std::string runId = "auto";
    std::string outDir = "out/sim";

    // Derived
    std::string fullOutDir;
};

static RunConfig g_config;

// Latency tracking
struct RpcRecord
{
    uint32_t seq;
    int64_t t_send_ns;
    int64_t t_recv_ns;
    int64_t lat_ns;
};

static std::vector<RpcRecord> g_rpcRecords;
static uint32_t g_completedRequests = 0;

// Event tracking (optional)
struct EventRecord
{
    int64_t t_ns;
    uint32_t node;
    std::string event;
    uint32_t seq;
    uint32_t len;
};

static std::vector<EventRecord> g_eventRecords;

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
    hash ^= std::hash<std::string>{}(g_config.workload);
    hash ^= g_config.outstanding * 31;
    hash ^= g_config.reqBytes * 37;

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

double
Percentile(std::vector<int64_t>& data, double p)
{
    if (data.empty()) return 0.0;

    std::sort(data.begin(), data.end());
    size_t idx = static_cast<size_t>(p * data.size());
    if (idx >= data.size()) idx = data.size() - 1;

    return static_cast<double>(data[idx]);
}

// ============================================================================
// Logging Functions
// ============================================================================

void
LogRpcRecord(uint32_t seq, int64_t t_send_ns, int64_t t_recv_ns)
{
    RpcRecord rec;
    rec.seq = seq;
    rec.t_send_ns = t_send_ns;
    rec.t_recv_ns = t_recv_ns;
    rec.lat_ns = t_recv_ns - t_send_ns;

    g_rpcRecords.push_back(rec);
    g_completedRequests++;
}

void
LogEvent(int64_t t_ns, uint32_t node, const std::string& event, uint32_t seq, uint32_t len)
{
    EventRecord rec;
    rec.t_ns = t_ns;
    rec.node = node;
    rec.event = event;
    rec.seq = seq;
    rec.len = len;

    g_eventRecords.push_back(rec);
}

void
WriteRpcLog()
{
    std::string path = g_config.fullOutDir + "/rpc.jsonl";
    std::ofstream ofs(path);

    if (!ofs.is_open())
    {
        NS_LOG_ERROR("Failed to open rpc.jsonl for writing");
        return;
    }

    for (const auto& rec : g_rpcRecords)
    {
        ofs << "{\"seq\":" << rec.seq
            << ",\"t_send_ns\":" << rec.t_send_ns
            << ",\"t_recv_ns\":" << rec.t_recv_ns
            << ",\"lat_ns\":" << rec.lat_ns
            << "}\n";
    }

    ofs.close();
    NS_LOG_INFO("Wrote " << g_rpcRecords.size() << " RPC records to rpc.jsonl");
}

void
WriteEventLog()
{
    std::string path = g_config.fullOutDir + "/events.jsonl";
    std::ofstream ofs(path);

    if (!ofs.is_open())
    {
        NS_LOG_ERROR("Failed to open events.jsonl for writing");
        return;
    }

    for (const auto& rec : g_eventRecords)
    {
        ofs << "{\"t_ns\":" << rec.t_ns
            << ",\"node\":" << rec.node
            << ",\"event\":\"" << rec.event << "\""
            << ",\"seq\":" << rec.seq
            << ",\"len\":" << rec.len
            << "}\n";
    }

    ofs.close();
    NS_LOG_INFO("Wrote " << g_eventRecords.size() << " event records to events.jsonl");
}

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
    ofs << "  \"qdisc\": \"" << g_config.qdisc << "\",\n";
    ofs << "  \"workload\": \"" << g_config.workload << "\",\n";
    ofs << "  \"nReq\": " << g_config.nReq << ",\n";
    ofs << "  \"outstanding\": " << g_config.outstanding << ",\n";
    ofs << "  \"reqBytes\": " << g_config.reqBytes << ",\n";
    ofs << "  \"rspBytes\": " << g_config.rspBytes << ",\n";
    ofs << "  \"enableEgressHook\": " << (g_config.enableEgressHook ? "true" : "false") << ",\n";
    ofs << "  \"enableIngressHook\": " << (g_config.enableIngressHook ? "true" : "false") << ",\n";
    ofs << "  \"hookConfigPath\": \"" << g_config.hookConfigPath << "\",\n";
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

    // Calculate statistics
    std::vector<int64_t> latencies;
    for (const auto& rec : g_rpcRecords)
    {
        latencies.push_back(rec.lat_ns);
    }

    double p50 = Percentile(latencies, 0.50);
    double p95 = Percentile(latencies, 0.95);
    double p99 = Percentile(latencies, 0.99);

    ofs << "CS538 Host Delay Experiment - Summary\n";
    ofs << "======================================\n\n";

    ofs << "Run ID: " << g_config.runId << "\n\n";

    ofs << "Configuration:\n";
    ofs << "--------------\n";
    ofs << "Workload:        " << g_config.workload << "\n";
    ofs << "Outstanding:     " << g_config.outstanding << "\n";
    ofs << "Request size:    " << g_config.reqBytes << " bytes\n";
    ofs << "Response size:   " << g_config.rspBytes << " bytes\n";
    ofs << "Link rate:       " << g_config.linkRate << "\n";
    ofs << "Link delay:      " << g_config.linkDelay << "\n";
    ofs << "MTU:             " << g_config.mtu << "\n";
    ofs << "Qdisc:           " << g_config.qdisc << "\n";
    ofs << "Egress hook:     " << (g_config.enableEgressHook ? "enabled" : "disabled") << "\n";
    ofs << "Ingress hook:    " << (g_config.enableIngressHook ? "enabled" : "disabled") << "\n";
    ofs << "Seed:            " << g_config.seed << "\n\n";

    ofs << "Results:\n";
    ofs << "--------\n";
    ofs << "Total requests:  " << g_config.nReq << "\n";
    ofs << "Completed:       " << g_completedRequests << "\n";
    ofs << "Loss:            " << (g_config.nReq - g_completedRequests) << "\n\n";

    ofs << "Latency (ns):\n";
    ofs << "  p50:           " << std::fixed << std::setprecision(0) << p50 << "\n";
    ofs << "  p95:           " << std::fixed << std::setprecision(0) << p95 << "\n";
    ofs << "  p99:           " << std::fixed << std::setprecision(0) << p99 << "\n\n";

    ofs << "Latency (μs):\n";
    ofs << "  p50:           " << std::fixed << std::setprecision(2) << (p50 / 1000.0) << "\n";
    ofs << "  p95:           " << std::fixed << std::setprecision(2) << (p95 / 1000.0) << "\n";
    ofs << "  p99:           " << std::fixed << std::setprecision(2) << (p99 / 1000.0) << "\n";

    ofs.close();
    NS_LOG_INFO("Wrote summary to summary.txt");

    // Also print to console
    std::cout << "\n=== Summary ===\n";
    std::cout << "Completed: " << g_completedRequests << "/" << g_config.nReq << "\n";
    std::cout << "p50: " << std::fixed << std::setprecision(2) << (p50 / 1000.0) << " μs\n";
    std::cout << "p95: " << std::fixed << std::setprecision(2) << (p95 / 1000.0) << " μs\n";
    std::cout << "p99: " << std::fixed << std::setprecision(2) << (p99 / 1000.0) << " μs\n";
}

// ============================================================================
// Custom RPC Application
// ============================================================================

class RpcClientApp : public Application
{
public:
    RpcClientApp();
    virtual ~RpcClientApp();

    void Setup(Address serverAddress,
               uint16_t port,
               uint32_t nReq,
               uint32_t outstanding,
               uint32_t reqSize,
               uint32_t rspSize);

private:
    virtual void StartApplication() override;
    virtual void StopApplication() override;

    void SendRequest();
    void HandleResponse(Ptr<Socket> socket);
    void ApplyEgressHook(uint32_t seq, uint32_t bytes);

    Ptr<Socket> m_socket;
    Address m_serverAddress;
    uint16_t m_port;
    uint32_t m_nReq;
    uint32_t m_outstanding;
    uint32_t m_reqSize;
    uint32_t m_rspSize;

    uint32_t m_sent;
    uint32_t m_received;
    uint32_t m_inFlight;

    std::map<uint32_t, int64_t> m_sendTimes;
};

RpcClientApp::RpcClientApp()
    : m_socket(nullptr),
      m_port(0),
      m_nReq(0),
      m_outstanding(1),
      m_reqSize(1024),
      m_rspSize(1024),
      m_sent(0),
      m_received(0),
      m_inFlight(0)
{
}

RpcClientApp::~RpcClientApp()
{
    m_socket = nullptr;
}

void
RpcClientApp::Setup(Address serverAddress,
                    uint16_t port,
                    uint32_t nReq,
                    uint32_t outstanding,
                    uint32_t reqSize,
                    uint32_t rspSize)
{
    m_serverAddress = serverAddress;
    m_port = port;
    m_nReq = nReq;
    m_outstanding = outstanding;
    m_reqSize = reqSize;
    m_rspSize = rspSize;
}

void
RpcClientApp::StartApplication()
{
    if (!m_socket)
    {
        TypeId tid = TypeId::LookupByName("ns3::UdpSocketFactory");
        m_socket = Socket::CreateSocket(GetNode(), tid);

        InetSocketAddress remote = InetSocketAddress(Ipv4Address::ConvertFrom(m_serverAddress), m_port);
        m_socket->Connect(remote);
        m_socket->SetRecvCallback(MakeCallback(&RpcClientApp::HandleResponse, this));
    }

    // Send initial batch of requests
    for (uint32_t i = 0; i < m_outstanding && m_sent < m_nReq; ++i)
    {
        SendRequest();
    }
}

void
RpcClientApp::StopApplication()
{
    if (m_socket)
    {
        m_socket->Close();
        m_socket->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
    }
}

void
RpcClientApp::ApplyEgressHook(uint32_t seq, uint32_t bytes)
{
    if (DelayHooks::IsEgressEnabled())
    {
        Time delay = DelayHooks::DelayEgress(GetNode()->GetId(), bytes, seq);
        if (delay.GetNanoSeconds() > 0)
        {
            Simulator::Schedule(delay, &RpcClientApp::SendRequest, this);
            return;
        }
    }
}

void
RpcClientApp::SendRequest()
{
    if (m_sent >= m_nReq || m_inFlight >= m_outstanding)
    {
        return;
    }

    uint32_t seq = m_sent++;
    m_inFlight++;

    // Create packet with sequence number in payload
    Ptr<Packet> packet = Create<Packet>(m_reqSize);

    // Record send time
    int64_t now_ns = Simulator::Now().GetNanoSeconds();
    m_sendTimes[seq] = now_ns;

    // Log event
    LogEvent(now_ns, GetNode()->GetId(), "tx_app", seq, m_reqSize);

    // Apply egress hook
    Time egressDelay = DelayHooks::DelayEgress(GetNode()->GetId(), m_reqSize, seq);
    if (egressDelay.GetNanoSeconds() > 0)
    {
        Simulator::Schedule(egressDelay, [this, packet, seq]() {
            LogEvent(Simulator::Now().GetNanoSeconds(), GetNode()->GetId(), "tx_post_egress", seq, m_reqSize);
            m_socket->Send(packet);
        });
    }
    else
    {
        LogEvent(now_ns, GetNode()->GetId(), "tx_post_egress", seq, m_reqSize);
        m_socket->Send(packet);
    }
}

void
RpcClientApp::HandleResponse(Ptr<Socket> socket)
{
    Ptr<Packet> packet;
    while ((packet = socket->Recv()))
    {
        uint32_t seq = m_received++;
        m_inFlight--;

        int64_t now_ns = Simulator::Now().GetNanoSeconds();

        // Apply ingress hook
        Time ingressDelay = DelayHooks::DelayIngress(GetNode()->GetId(), packet->GetSize(), seq);

        int64_t recv_ns = now_ns + ingressDelay.GetNanoSeconds();

        LogEvent(now_ns, GetNode()->GetId(), "rx_nic", seq, packet->GetSize());
        LogEvent(recv_ns, GetNode()->GetId(), "rx_post_ingress", seq, packet->GetSize());

        // Log RPC completion
        if (m_sendTimes.find(seq) != m_sendTimes.end())
        {
            LogRpcRecord(seq, m_sendTimes[seq], recv_ns);
            m_sendTimes.erase(seq);
        }

        // Send next request if we haven't reached limit
        if (m_sent < m_nReq)
        {
            SendRequest();
        }

        // Check if we're done
        if (m_received >= m_nReq)
        {
            Simulator::Stop();
        }
    }
}

// ============================================================================
// Simple RPC Server Application
// ============================================================================

class RpcServerApp : public Application
{
public:
    RpcServerApp();
    virtual ~RpcServerApp();

    void Setup(uint16_t port, uint32_t rspSize);

private:
    virtual void StartApplication() override;
    virtual void StopApplication() override;

    void HandleRequest(Ptr<Socket> socket);

    Ptr<Socket> m_socket;
    uint16_t m_port;
    uint32_t m_rspSize;
};

RpcServerApp::RpcServerApp()
    : m_socket(nullptr),
      m_port(0),
      m_rspSize(1024)
{
}

RpcServerApp::~RpcServerApp()
{
    m_socket = nullptr;
}

void
RpcServerApp::Setup(uint16_t port, uint32_t rspSize)
{
    m_port = port;
    m_rspSize = rspSize;
}

void
RpcServerApp::StartApplication()
{
    if (!m_socket)
    {
        TypeId tid = TypeId::LookupByName("ns3::UdpSocketFactory");
        m_socket = Socket::CreateSocket(GetNode(), tid);
        InetSocketAddress local = InetSocketAddress(Ipv4Address::GetAny(), m_port);
        m_socket->Bind(local);
        m_socket->SetRecvCallback(MakeCallback(&RpcServerApp::HandleRequest, this));
    }
}

void
RpcServerApp::StopApplication()
{
    if (m_socket)
    {
        m_socket->Close();
        m_socket->SetRecvCallback(MakeNullCallback<void, Ptr<Socket>>());
    }
}

void
RpcServerApp::HandleRequest(Ptr<Socket> socket)
{
    Ptr<Packet> packet;
    Address from;

    while ((packet = socket->RecvFrom(from)))
    {
        // Immediately send response
        Ptr<Packet> response = Create<Packet>(m_rspSize);
        socket->SendTo(response, 0, from);
    }
}

// ============================================================================
// Topology Setup
// ============================================================================

void
SetupTopology(NodeContainer& hosts, Ipv4InterfaceContainer& interfaces)
{
    NS_LOG_INFO("Setting up Host0 → Switch → Host1 topology");

    // Create 3 nodes: Host0, Switch, Host1
    NodeContainer allNodes;
    allNodes.Create(3);

    hosts.Add(allNodes.Get(0));  // Host0
    hosts.Add(allNodes.Get(2));  // Host1
    // allNodes.Get(1) is the Switch

    // Configure point-to-point links
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(g_config.linkRate));
    p2p.SetChannelAttribute("Delay", StringValue(g_config.linkDelay));
    p2p.SetDeviceAttribute("Mtu", UintegerValue(g_config.mtu));

    // For simplicity, we'll create a direct link between Host0 and Host1
    // (In a real switch topology, we'd use a bridge, but for deterministic
    // behavior, a direct link is cleaner)
    NetDeviceContainer devices = p2p.Install(hosts);

    // Install Internet stack
    InternetStackHelper stack;
    stack.Install(hosts);

    // Assign IP addresses
    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    interfaces = address.Assign(devices);

    NS_LOG_INFO("Topology setup complete");
    NS_LOG_INFO("  Host0: " << interfaces.GetAddress(0));
    NS_LOG_INFO("  Host1: " << interfaces.GetAddress(1));
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
    cmd.AddValue("qdisc", "Queue discipline (none|fq_codel)", g_config.qdisc);

    // Workload parameters
    cmd.AddValue("workload", "Workload type (pingpong|rpc)", g_config.workload);
    cmd.AddValue("nReq", "Number of requests", g_config.nReq);
    cmd.AddValue("outstanding", "Outstanding requests", g_config.outstanding);
    cmd.AddValue("reqBytes", "Request size in bytes", g_config.reqBytes);
    cmd.AddValue("rspBytes", "Response size in bytes", g_config.rspBytes);

    // Hook parameters
    cmd.AddValue("enableEgressHook", "Enable egress hook", g_config.enableEgressHook);
    cmd.AddValue("enableIngressHook", "Enable ingress hook", g_config.enableIngressHook);
    cmd.AddValue("hookConfigPath", "Path to hook config file", g_config.hookConfigPath);

    // Simulation parameters
    cmd.AddValue("seed", "Random seed", g_config.seed);
    cmd.AddValue("runId", "Run ID (auto or custom)", g_config.runId);
    cmd.AddValue("outDir", "Output directory", g_config.outDir);

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

    NS_LOG_INFO("CS538 Host Delay Experiment Runner");
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

    // Setup applications
    uint16_t port = 9999;

    // Server on Host1
    Ptr<RpcServerApp> serverApp = CreateObject<RpcServerApp>();
    serverApp->Setup(port, g_config.rspBytes);
    hosts.Get(1)->AddApplication(serverApp);
    serverApp->SetStartTime(Seconds(0.0));
    serverApp->SetStopTime(Seconds(1000.0));

    // Client on Host0
    Ptr<RpcClientApp> clientApp = CreateObject<RpcClientApp>();
    clientApp->Setup(interfaces.GetAddress(1),
                     port,
                     g_config.nReq,
                     g_config.outstanding,
                     g_config.reqBytes,
                     g_config.rspBytes);
    hosts.Get(0)->AddApplication(clientApp);
    clientApp->SetStartTime(Seconds(0.1));
    clientApp->SetStopTime(Seconds(1000.0));

    NS_LOG_INFO("Starting simulation");
    NS_LOG_INFO("  Workload: " << g_config.workload);
    NS_LOG_INFO("  Requests: " << g_config.nReq);
    NS_LOG_INFO("  Outstanding: " << g_config.outstanding);
    NS_LOG_INFO("  Req/Rsp size: " << g_config.reqBytes << "/" << g_config.rspBytes);

    // Run simulation
    Simulator::Stop(Seconds(60.0));  // Max 60s
    Simulator::Run();

    NS_LOG_INFO("Simulation complete");

    // Write outputs
    WriteConfigLog();
    WriteRpcLog();
    WriteEventLog();
    WriteSummary();

    Simulator::Destroy();

    std::cout << "\nResults written to: " << g_config.fullOutDir << "\n";

    return 0;
}
