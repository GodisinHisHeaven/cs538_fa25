#include "delay_hooks.h"
#include "ns3/abort.h"
#include "ns3/config.h"
#include "ns3/log.h"
#include "ns3/names.h"
#include "ns3/net-device-queue-interface.h"
#include "ns3/packet.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/simulator.h"

#include <map>

#ifdef NS3_MPI
#include "ns3/mpi-interface.h"
#include "ns3/mpi-receiver.h"
#include "ns3/point-to-point-remote-channel.h"
#endif

#include "point-to-point-helper-custom.h"

#include "ns3/trace-helper.h"

namespace ns3
{

// ============================================================================
// DelayedPointToPointChannel Definition
// ============================================================================
class DelayedPointToPointChannel : public PointToPointChannel
{
public:
    static TypeId GetTypeId();
    virtual bool TransmitStart(Ptr<const Packet> p,
                              Ptr<PointToPointNetDevice> src,
                              Time txTime);
private:
    // Track sequence numbers for egress hook
    std::map<uint32_t, uint32_t> m_egressSeq;
};

TypeId
DelayedPointToPointChannel::GetTypeId()
{
    static TypeId tid = TypeId("DelayedPointToPointChannel")
                            .SetParent<PointToPointChannel>()
                            .SetGroupName("PointToPoint")
                            .AddConstructor<DelayedPointToPointChannel>();
    return tid;
}

bool
DelayedPointToPointChannel::TransmitStart(Ptr<const Packet> p,
                                         Ptr<PointToPointNetDevice> src,
                                         Time txTime)
{

    // Find the destination device (the other device on this channel)
    // A point-to-point channel has exactly 2 devices
    Ptr<PointToPointNetDevice> dst = nullptr;
    if (GetNDevices() != 2)
    {
        return false;
    }

    // Find which device is NOT the source
    Ptr<NetDevice> dev0 = GetDevice(0);
    Ptr<NetDevice> dev1 = GetDevice(1);

    if (dev0 == src)
    {
        dst = DynamicCast<PointToPointNetDevice>(dev1);
    }
    else if (dev1 == src)
    {
        dst = DynamicCast<PointToPointNetDevice>(dev0);
    }
    else
    {
        return false;
    }

    if (!dst)
    {
        return false;
    }

    // Calculate egress delay if hook is enabled
    Time egressDelay = NanoSeconds(0);
    if (DelayHooks::IsEgressEnabled())
    {
        uint32_t srcNodeId = src->GetNode()->GetId();
        uint32_t seq = m_egressSeq[srcNodeId]++;
        uint32_t bytes = p->GetSize();
        egressDelay = DelayHooks::DelayEgress(srcNodeId, bytes, seq);

    }

    // Get propagation delay from channel
    Time propDelay = GetDelay();

    // Total delay = egress delay + propagation delay + transmission time
    Time totalDelay = egressDelay + propDelay + txTime;

    // Schedule packet reception at destination with correct node context
    // Using ScheduleWithContext ensures the simulator context is set to the destination node
    Simulator::ScheduleWithContext(dst->GetNode()->GetId(),
                                   totalDelay,
                                   &PointToPointNetDevice::Receive,
                                   dst,
                                   p->Copy());

    return true;
}

NS_OBJECT_ENSURE_REGISTERED(DelayedPointToPointChannel);

NS_LOG_COMPONENT_DEFINE("PointToPointHelperCustom");

PointToPointHelperCustom::PointToPointHelperCustom()
{
    m_queueFactory.SetTypeId("ns3::DropTailQueue<Packet>");
    m_deviceFactory.SetTypeId("ns3::PointToPointNetDevice");
    m_channelFactory.SetTypeId("DelayedPointToPointChannel");
    m_enableFlowControl = true;
}

void
PointToPointHelperCustom::SetDeviceAttribute(std::string n1, const AttributeValue& v1)
{
    m_deviceFactory.Set(n1, v1);
}

void
PointToPointHelperCustom::SetChannelAttribute(std::string n1, const AttributeValue& v1)
{
    m_channelFactory.Set(n1, v1);
}

void
PointToPointHelperCustom::DisableFlowControl()
{
    m_enableFlowControl = false;
}

void
PointToPointHelperCustom::EnablePcapInternal(std::string prefix,
                                       Ptr<NetDevice> nd,
                                       bool promiscuous,
                                       bool explicitFilename)
{
    //
    // All of the Pcap enable functions vector through here including the ones
    // that are wandering through all of devices on perhaps all of the nodes in
    // the system.  We can only deal with devices of type PointToPointNetDevice.
    //
    Ptr<PointToPointNetDevice> device = nd->GetObject<PointToPointNetDevice>();
    if (!device)
    {
        NS_LOG_INFO("Device " << device << " not of type ns3::PointToPointNetDevice");
        return;
    }

    PcapHelper pcapHelper;

    std::string filename;
    if (explicitFilename)
    {
        filename = prefix;
    }
    else
    {
        filename = pcapHelper.GetFilenameFromDevice(prefix, device);
    }

    Ptr<PcapFileWrapper> file = pcapHelper.CreateFile(filename, std::ios::out, PcapHelper::DLT_PPP);
    pcapHelper.HookDefaultSink<PointToPointNetDevice>(device, "PromiscSniffer", file);
}

void
PointToPointHelperCustom::EnableAsciiInternal(Ptr<OutputStreamWrapper> stream,
                                        std::string prefix,
                                        Ptr<NetDevice> nd,
                                        bool explicitFilename)
{
    //
    // All of the ascii enable functions vector through here including the ones
    // that are wandering through all of devices on perhaps all of the nodes in
    // the system.  We can only deal with devices of type PointToPointNetDevice.
    //
    Ptr<PointToPointNetDevice> device = nd->GetObject<PointToPointNetDevice>();
    if (!device)
    {
        NS_LOG_INFO("Device " << device << " not of type ns3::PointToPointNetDevice");
        return;
    }

    //
    // Our default trace sinks are going to use packet printing, so we have to
    // make sure that is turned on.
    //
    Packet::EnablePrinting();

    //
    // If we are not provided an OutputStreamWrapper, we are expected to create
    // one using the usual trace filename conventions and do a Hook*WithoutContext
    // since there will be one file per context and therefore the context would
    // be redundant.
    //
    if (!stream)
    {
        //
        // Set up an output stream object to deal with private ofstream copy
        // constructor and lifetime issues.  Let the helper decide the actual
        // name of the file given the prefix.
        //
        AsciiTraceHelper asciiTraceHelper;

        std::string filename;
        if (explicitFilename)
        {
            filename = prefix;
        }
        else
        {
            filename = asciiTraceHelper.GetFilenameFromDevice(prefix, device);
        }

        Ptr<OutputStreamWrapper> theStream = asciiTraceHelper.CreateFileStream(filename);

        //
        // The MacRx trace source provides our "r" event.
        //
        asciiTraceHelper.HookDefaultReceiveSinkWithoutContext<PointToPointNetDevice>(device,
                                                                                     "MacRx",
                                                                                     theStream);

        //
        // The "+", '-', and 'd' events are driven by trace sources actually in the
        // transmit queue.
        //
        Ptr<Queue<Packet>> queue = device->GetQueue();
        asciiTraceHelper.HookDefaultEnqueueSinkWithoutContext<Queue<Packet>>(queue,
                                                                             "Enqueue",
                                                                             theStream);
        asciiTraceHelper.HookDefaultDropSinkWithoutContext<Queue<Packet>>(queue, "Drop", theStream);
        asciiTraceHelper.HookDefaultDequeueSinkWithoutContext<Queue<Packet>>(queue,
                                                                             "Dequeue",
                                                                             theStream);

        // PhyRxDrop trace source for "d" event
        asciiTraceHelper.HookDefaultDropSinkWithoutContext<PointToPointNetDevice>(device,
                                                                                  "PhyRxDrop",
                                                                                  theStream);

        return;
    }

    //
    // If we are provided an OutputStreamWrapper, we are expected to use it, and
    // to providd a context.  We are free to come up with our own context if we
    // want, and use the AsciiTraceHelper Hook*WithContext functions, but for
    // compatibility and simplicity, we just use Config::Connect and let it deal
    // with the context.
    //
    // Note that we are going to use the default trace sinks provided by the
    // ascii trace helper.  There is actually no AsciiTraceHelper in sight here,
    // but the default trace sinks are actually publicly available static
    // functions that are always there waiting for just such a case.
    //
    uint32_t nodeid = nd->GetNode()->GetId();
    uint32_t deviceid = nd->GetIfIndex();
    std::ostringstream oss;

    oss << "/NodeList/" << nodeid << "/DeviceList/" << deviceid
        << "/$ns3::PointToPointNetDevice/MacRx";
    Config::Connect(oss.str(),
                    MakeBoundCallback(&AsciiTraceHelper::DefaultReceiveSinkWithContext, stream));

    oss.str("");
    oss << "/NodeList/" << nodeid << "/DeviceList/" << deviceid
        << "/$ns3::PointToPointNetDevice/TxQueue/Enqueue";
    Config::Connect(oss.str(),
                    MakeBoundCallback(&AsciiTraceHelper::DefaultEnqueueSinkWithContext, stream));

    oss.str("");
    oss << "/NodeList/" << nodeid << "/DeviceList/" << deviceid
        << "/$ns3::PointToPointNetDevice/TxQueue/Dequeue";
    Config::Connect(oss.str(),
                    MakeBoundCallback(&AsciiTraceHelper::DefaultDequeueSinkWithContext, stream));

    oss.str("");
    oss << "/NodeList/" << nodeid << "/DeviceList/" << deviceid
        << "/$ns3::PointToPointNetDevice/TxQueue/Drop";
    Config::Connect(oss.str(),
                    MakeBoundCallback(&AsciiTraceHelper::DefaultDropSinkWithContext, stream));

    oss.str("");
    oss << "/NodeList/" << nodeid << "/DeviceList/" << deviceid
        << "/$ns3::PointToPointNetDevice/PhyRxDrop";
    Config::Connect(oss.str(),
                    MakeBoundCallback(&AsciiTraceHelper::DefaultDropSinkWithContext, stream));
}

NetDeviceContainer
PointToPointHelperCustom::Install(NodeContainer c)
{
    NS_ASSERT(c.GetN() == 2);
    return Install(c.Get(0), c.Get(1));
}

NetDeviceContainer
PointToPointHelperCustom::Install(Ptr<Node> a, Ptr<Node> b)
{
    NetDeviceContainer container;

    Ptr<PointToPointNetDevice> devA = m_deviceFactory.Create<PointToPointNetDevice>();
    devA->SetAddress(Mac48Address::Allocate());
    a->AddDevice(devA);
    Ptr<Queue<Packet>> queueA = m_queueFactory.Create<Queue<Packet>>();
    devA->SetQueue(queueA);
    Ptr<PointToPointNetDevice> devB = m_deviceFactory.Create<PointToPointNetDevice>();
    devB->SetAddress(Mac48Address::Allocate());
    b->AddDevice(devB);
    Ptr<Queue<Packet>> queueB = m_queueFactory.Create<Queue<Packet>>();
    devB->SetQueue(queueB);
    if (m_enableFlowControl)
    {
        // Aggregate NetDeviceQueueInterface objects
        Ptr<NetDeviceQueueInterface> ndqiA = CreateObject<NetDeviceQueueInterface>();
        ndqiA->GetTxQueue(0)->ConnectQueueTraces(queueA);
        devA->AggregateObject(ndqiA);
        Ptr<NetDeviceQueueInterface> ndqiB = CreateObject<NetDeviceQueueInterface>();
        ndqiB->GetTxQueue(0)->ConnectQueueTraces(queueB);
        devB->AggregateObject(ndqiB);
    }

    Ptr<DelayedPointToPointChannel> channel = nullptr;

    // If MPI is enabled, we need to see if both nodes have the same system id
    // (rank), and the rank is the same as this instance.  If both are true,
    // use a normal p2p channel, otherwise use a remote channel
#ifdef NS3_MPI
    bool useNormalChannel = true;
    if (MpiInterface::IsEnabled())
    {
        uint32_t n1SystemId = a->GetSystemId();
        uint32_t n2SystemId = b->GetSystemId();
        uint32_t currSystemId = MpiInterface::GetSystemId();
        if (n1SystemId != currSystemId || n2SystemId != currSystemId)
        {
            useNormalChannel = false;
        }
    }
    if (useNormalChannel)
    {
        m_channelFactory.SetTypeId("DelayedPointToPointChannel");
        channel = m_channelFactory.Create<DelayedPointToPointChannel>();
    }
    else
    {
        m_channelFactory.SetTypeId("ns3::PointToPointRemoteChannel");
        channel = m_channelFactory.Create<PointToPointRemoteChannel>();
        Ptr<MpiReceiver> mpiRecA = CreateObject<MpiReceiver>();
        Ptr<MpiReceiver> mpiRecB = CreateObject<MpiReceiver>();
        mpiRecA->SetReceiveCallback(MakeCallback(&PointToPointNetDevice::Receive, devA));
        mpiRecB->SetReceiveCallback(MakeCallback(&PointToPointNetDevice::Receive, devB));
        devA->AggregateObject(mpiRecA);
        devB->AggregateObject(mpiRecB);
    }
#else
    channel = m_channelFactory.Create<DelayedPointToPointChannel>();
#endif

    devA->Attach(channel);
    devB->Attach(channel);
    container.Add(devA);
    container.Add(devB);

    return container;
}

NetDeviceContainer
PointToPointHelperCustom::Install(Ptr<Node> a, std::string bName)
{
    Ptr<Node> b = Names::Find<Node>(bName);
    return Install(a, b);
}

NetDeviceContainer
PointToPointHelperCustom::Install(std::string aName, Ptr<Node> b)
{
    Ptr<Node> a = Names::Find<Node>(aName);
    return Install(a, b);
}

NetDeviceContainer
PointToPointHelperCustom::Install(std::string aName, std::string bName)
{
    Ptr<Node> a = Names::Find<Node>(aName);
    Ptr<Node> b = Names::Find<Node>(bName);
    return Install(a, b);
}

} // namespace ns3
