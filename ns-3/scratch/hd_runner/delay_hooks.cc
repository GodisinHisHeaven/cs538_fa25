/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * CS538 Host Delay Hooks - Cache Miss + Queueing Delay Model
 *
 * Model: Host_delay = base_CPU_cycles + (cache_misses × penalty_P(L)) + queueing_delay_Q(L)
 * Where L is the load factor calculated from rolling packet rate
 *
 * Features:
 * - Probabilistic cache miss model based on packet size
 * - Rolling window load tracking per node
 * - Step function for load-dependent penalty and queueing delay
 * - Configurable severity multiplier for experiments
 */

#include "delay_hooks.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <deque>
#include <map>
#include <cmath>
#include <random>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("DelayHooks");

// ============================================================================
// Default Delay Model (No delay injection)
// ============================================================================

class DefaultDelayModel: public DelayModel
{
public:
    std::string GetName() const override
    {
        return "Default";
    }

    void Initialize(const std::string& config, uint32_t seed) override {}
    void Reset() override {}
    
    Time CalculateEgressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq)
    {
        return Time(0);
    }
    Time CalculateIngressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq)
    {
        return Time(0);
    }

};

// ============================================================================
// CacheMiss Delay Model
// ============================================================================

struct CacheMissConfig
{
    // Base parameters
    uint32_t baseCpuCyclesNs;
    double severityMultiplier;

    // Cache model
    struct {
        uint32_t cacheLineSize;
        uint32_t smallPacketThreshold;
        uint32_t largePacketThreshold;
        double missProbabilitySmall;
        double missProbabilityMedium;
        double missProbabilityLarge;
    } cache;

    // Penalty model (step function)
    struct {
        uint32_t basePenaltyNs;
        double loadThreshold;
        double highLoadMultiplier;
    } penalty;

    // Queue model (step function with growth)
    struct {
        uint32_t baseQueueNs;
        double loadThreshold;
        double queueGrowthFactor;
    } queue;

    // Load tracking
    struct {
        uint32_t windowSizeMs;
        double nominalRatePps;
    } loadTracking;
};

class CacheMissDelayModel: public DelayModel
{
    std::string GetName() const override
    {
        return "CacheMiss";
    }

    void Initialize(const std::string& config, uint32_t seed) override 
    {
        m_seed = seed;

        // Load configuration based on config path
        // For now, use simple profile selection based on config string
        if (config.empty() || config == "realistic")
        {
            m_config = CreateRealisticConfig();
            NS_LOG_INFO("  Using REALISTIC config (severity=1.0, ~2us impact)");
        }
        else if (config == "moderate")
        {
            m_config = CreateModerateConfig();
            NS_LOG_INFO("  Using MODERATE config (severity=3.0, ~6us impact)");
        }
        else if (config == "severe")
        {
            m_config = CreateSevereConfig();
            NS_LOG_INFO("  Using SEVERE config (severity=10.0, ~20us impact)");
        }
        else
        {
            // Default to realistic
            m_config = CreateRealisticConfig();
            NS_LOG_INFO("  Unknown config '" << config << "', using REALISTIC");
        }

        //Log for debugging
        NS_LOG_INFO("DelayHooks initialized:");
        NS_LOG_INFO("  Config: " << (config.empty() ? "realistic (default)" : config));
        NS_LOG_INFO("  Seed: " << seed);
        NS_LOG_INFO("  Model: base=" << m_config.baseCpuCyclesNs << "ns"
                    << " severity=" << m_config.severityMultiplier
                    << " penalty_threshold=" << m_config.penalty.loadThreshold
                    << " queue_threshold=" << m_config.queue.loadThreshold);
    }

    void Reset() override
    {
        m_nodeTimestamps.clear();
        m_packetCount = 0;
    }

    Time CalculateEgressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq)
    {
        int64_t nowNs = Simulator::Now().GetNanoSeconds();

        // Calculate load factor
        double load = CalculateLoad(nodeId, nowNs);

        // Calculate cache misses
        uint32_t cacheMisses = CalculateCacheMisses(bytes);

        // Calculate penalty (load-dependent)
        uint32_t penaltyPerMiss = CalculatePenalty(load);

        // Calculate queueing delay (load-dependent)
        uint32_t queueDelay = CalculateQueueDelay(load);

        // Total delay = base + (misses × penalty) + queue
        uint32_t totalDelayNs = m_config.baseCpuCyclesNs +
                                (cacheMisses * penaltyPerMiss) +
                                queueDelay;

        NS_LOG_DEBUG("DelayEgress: node=" << nodeId
                    << " bytes=" << bytes
                    << " seq=" << seq
                    << " load=" << load
                    << " misses=" << cacheMisses
                    << " penalty=" << penaltyPerMiss
                    << " queue=" << queueDelay
                    << " totalDelay=" << totalDelayNs << "ns");

        // Print summary every 1000 packets for visibility
        // m_packetCount++;
        // if (m_packetCount % 1000 == 0)
        // {
        //     std::cout << "Egress delay (pkt " << m_packetCount << "): "
        //             << "load=" << load
        //             << " misses=" << cacheMisses
        //             << " delay=" << totalDelayNs << "ns" << std::endl;
        // }
        return NanoSeconds(totalDelayNs);
    }

    Time CalculateIngressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq)
    {
        int64_t nowNs = Simulator::Now().GetNanoSeconds();

        // Calculate load factor
        double load = CalculateLoad(nodeId, nowNs);

        // Calculate cache misses
        uint32_t cacheMisses = CalculateCacheMisses(bytes);

        // Calculate penalty (load-dependent)
        uint32_t penaltyPerMiss = CalculatePenalty(load);

        // Calculate queueing delay (load-dependent)
        uint32_t queueDelay = CalculateQueueDelay(load);

        // Total delay = base + (misses × penalty) + queue
        uint32_t totalDelayNs = m_config.baseCpuCyclesNs +
                                (cacheMisses * penaltyPerMiss) +
                                queueDelay;

        NS_LOG_DEBUG("DelayIngress: node=" << nodeId
                    << " bytes=" << bytes
                    << " seq=" << seq
                    << " load=" << load
                    << " misses=" << cacheMisses
                    << " penalty=" << penaltyPerMiss
                    << " queue=" << queueDelay
                    << " totalDelay=" << totalDelayNs << "ns");

        // Print summary every 1000 packets for visibility
        // static uint32_t m_packetCount = 0;
        // m_packetCount++;
        // if (m_packetCount % 1000 == 0)
        // {
        //     std::cout << "Ingress delay (pkt " << m_packetCount << "): "
        //             << "load=" << load
        //             << " misses=" << cacheMisses
        //             << " delay=" << totalDelayNs << "ns" << std::endl;
        // }

        return NanoSeconds(totalDelayNs);
    }
    
private:
    CacheMissConfig m_config;
    uint32_t m_seed = 0;
    uint32_t m_packetCount = 0;
    std::map<uint32_t, std::deque<int64_t>> m_nodeTimestamps; // Per-node timestamp tracking for load calculation


    // Default configuration profiles
    static CacheMissConfig CreateRealisticConfig()
    {
        CacheMissConfig config;
        config.baseCpuCyclesNs = 150;
        config.severityMultiplier = 1.0;

        config.cache.cacheLineSize = 64;
        config.cache.smallPacketThreshold = 256;
        config.cache.largePacketThreshold = 1024;
        config.cache.missProbabilitySmall = 0.1;
        config.cache.missProbabilityMedium = 0.4;
        config.cache.missProbabilityLarge = 0.7;

        config.penalty.basePenaltyNs = 100;
        config.penalty.loadThreshold = 0.7;
        config.penalty.highLoadMultiplier = 3.0;

        config.queue.baseQueueNs = 50;
        config.queue.loadThreshold = 0.6;
        config.queue.queueGrowthFactor = 2.0;

        config.loadTracking.windowSizeMs = 100;
        config.loadTracking.nominalRatePps = 10000.0;

        return config;
    }

    static CacheMissConfig CreateModerateConfig()
    {
        CacheMissConfig config = CreateRealisticConfig();
        config.severityMultiplier = 3.0;
        return config;
    }

    static CacheMissConfig CreateSevereConfig()
    {
        CacheMissConfig config = CreateRealisticConfig();
        config.severityMultiplier = 10.0;
        return config;
    }

    /**
     * Calculate cache misses probabilistically based on packet size
     *
     * Small packets (<= 256B): Low miss probability (0.1)
     * Medium packets (256B-1KB): Medium miss probability (0.4)
     * Large packets (> 1KB): High miss probability (0.7)
     *
     * Returns estimated number of cache line misses
     */
    uint32_t CalculateCacheMisses(uint32_t bytes)
    {
        double missProb;
        if (bytes <= m_config.cache.smallPacketThreshold)
        {
            missProb = m_config.cache.missProbabilitySmall;
        }
        else if (bytes <= m_config.cache.largePacketThreshold)
        {
            missProb = m_config.cache.missProbabilityMedium;
        }
        else
        {
            missProb = m_config.cache.missProbabilityLarge;
        }

        // Calculate number of cache lines this packet spans
        uint32_t cacheLines = (bytes + m_config.cache.cacheLineSize - 1) /
                            m_config.cache.cacheLineSize;

        // Apply probability to get expected cache misses
        uint32_t cacheMisses = static_cast<uint32_t>(std::ceil(cacheLines * missProb));

        NS_LOG_DEBUG("CalculateCacheMisses: bytes=" << bytes
                    << " cacheLines=" << cacheLines
                    << " missProb=" << missProb
                    << " misses=" << cacheMisses);

        return cacheMisses;
    }

    /**
     * Calculate current load factor from rolling window of packet timestamps
     *
     * Load = (packets in window) / (window duration × nominal rate)
     *
     * Returns load factor (0.0 = no load, 1.0 = nominal load, >1.0 = overload)
     */
    double CalculateLoad(uint32_t nodeId, int64_t currentTimeNs)
    {
        auto& timestamps = m_nodeTimestamps[nodeId];

        // Remove expired timestamps (older than window)
        int64_t windowNs = static_cast<int64_t>(m_config.loadTracking.windowSizeMs) * 1000000LL;
        int64_t cutoffTime = currentTimeNs - windowNs;

        while (!timestamps.empty() && timestamps.front() < cutoffTime)
        {
            timestamps.pop_front();
        }

        // Add current packet timestamp
        timestamps.push_back(currentTimeNs);

        // Calculate load
        double packetsInWindow = static_cast<double>(timestamps.size());
        double windowDurationS = static_cast<double>(windowNs) / 1e9;
        double actualRate = packetsInWindow / windowDurationS;
        double load = actualRate / m_config.loadTracking.nominalRatePps;

        NS_LOG_DEBUG("CalculateLoad: node=" << nodeId
                    << " packetsInWindow=" << packetsInWindow
                    << " actualRate=" << actualRate
                    << " load=" << load);

        return load;
    }

    /**
     * Calculate cache miss penalty based on load (step function)
     *
     * Below threshold: Base penalty
     * Above threshold: Base penalty × multiplier
     *
     * Returns penalty per cache miss in nanoseconds
     */
    uint32_t CalculatePenalty(double load)
    {
        uint32_t penalty = m_config.penalty.basePenaltyNs;

        if (load >= m_config.penalty.loadThreshold)
        {
            penalty = static_cast<uint32_t>(penalty * m_config.penalty.highLoadMultiplier);
            NS_LOG_DEBUG("CalculatePenalty: HIGH LOAD - load=" << load
                        << " threshold=" << m_config.penalty.loadThreshold
                        << " penalty=" << penalty);
        }
        else
        {
            NS_LOG_DEBUG("CalculatePenalty: normal load - load=" << load
                        << " penalty=" << penalty);
        }

        return static_cast<uint32_t>(penalty * m_config.severityMultiplier);
    }

    /**
     * Calculate queueing delay based on load (step function with growth)
     *
     * Below threshold: Base queue delay
     * Above threshold: Base delay + growth based on excess load
     *
     * Returns queueing delay in nanoseconds
     */
    uint32_t CalculateQueueDelay(double load)
    {
        uint32_t queueDelay = m_config.queue.baseQueueNs;

        if (load >= m_config.queue.loadThreshold)
        {
            double excessLoad = load - m_config.queue.loadThreshold;
            uint32_t additionalDelay = static_cast<uint32_t>(
                m_config.queue.baseQueueNs * m_config.queue.queueGrowthFactor * excessLoad
            );
            queueDelay += additionalDelay;

            NS_LOG_DEBUG("CalculateQueueDelay: HIGH LOAD - load=" << load
                        << " excessLoad=" << excessLoad
                        << " queueDelay=" << queueDelay);
        }
        else
        {
            NS_LOG_DEBUG("CalculateQueueDelay: normal load - load=" << load
                        << " queueDelay=" << queueDelay);
        }

        return static_cast<uint32_t>(queueDelay * m_config.severityMultiplier);
    }
};


class UHNDelayModel: public DelayModel
{
public:
    std::string GetName() const override
    {
        return "UHN";
    }

    void Initialize(const std::string& config, uint32_t seed) override {}
    void Reset() override {}
    
    Time CalculateEgressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq)
    {
        NodeProperties props = GetNodeProperties(nodeId);

        uint32_t p2mWriteConstantLatency = 300;


        uint32_t switchingDelay = 1 * 0.50 * 10 //averageRpqOccupancy * numSwitches  / linesRead * timeToSwitchWriteToRead;
        uint32_t writeHolBlocking = averageRpqOccupancy * linesWritten / linesRead * timeToTransmit;
        uint32_t readHolBlocking = (averageRpqOccupancy - 1) * timeToTransmit;
        uint32_t topOfQueueDelay = numActRead / linesRead * timeToAct + numPreConflictRead / linesRead * timeToPre;

        uint32_t lRead = constantRead + switchingDelay + writeHolBlocking + readHolBlocking + topOfQueueDelay;

        //ReadWrite caused by reading to cache during a write and writing back later
        // P2M write uses this
        // C2M-Write Domain: LFB -> CHA (credit allocated at LFB and replenished at CHA)
        // - Degradation in tput caused by DRAM row miss ratio and load imbalance across banks
        // P2M-Write Domain: IIO -> CHA -> MC (credit allocated at IIO and replenished at MC)
        // - latency inflation in P2M-Write Domain latency (~20-30ns)
        // - with 3-4 cores C2M start to reduce P2M tput as memory bandwidth gets saurated (MC write queue full) leading to backlog of writes at CHA
        // - domain inflation only for P2M-write domain (3-4 C2M cores result in latency increase 1.5x) and all domain credits used (92)
        // C2M ReadWrite bound by C2M Read domain latency (~12% increase) as C2M write domain latency does not increase
        // As write backlog at CHAT continue to increas with >4C2M cores, CHA begins to apply backpressure (RPQ capped impacting both domains 50ns)
        uint32_t switchingDelay = numWriteRequestsWaiting * numSwitches  / linesWritten * timeToSwitchReadToWrite;
        uint32_t readHolBlocking = numWriteRequestsWaiting * linesRead / linesWritten * timeToTransmit;
        uint32_t writeHolBlocking = (numWriteRequestsWaiting - 1) * timeToTransmit;
        uint32_t topOfQueueDelay = numActWrite / linesWritten * timeToAct + numPreConflictWrite / linesWritten * timeToPre;

        uint32_t lWrite = constantWrite + probabilityWpqFull * (switchingDelay + readHolBlocking + writeHolBlocking + topOfQueueDelay); 
        
        
        // // Scale delay based on CPU contention
        // uint32_t baseDelay = 1000000;  // ns
        // uint32_t contentionMultiplier = 1 + props.cpuCoreContention;

        // // std::cout <<  "contentionMulti: " << contentionMultiplier << "\n";
        
        // return NanoSeconds(baseDelay * contentionMultiplier);
    }
    Time CalculateIngressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq)
    {
        return Time(0);
    }

};

// ============================================================================
// DelayHooks Impl
// ============================================================================

// Static member initialization
bool DelayHooks::s_egressEnabled = false;
bool DelayHooks::s_ingressEnabled = false;
std::unique_ptr<DelayModel> DelayHooks::s_model = nullptr;

void DelayHooks::Initialize(const std::string& modelName,
                       const std::string& config,
                       bool enableEgress,
                       bool enableIngress,
                       uint32_t seed)
{
    s_egressEnabled = enableEgress;
    s_ingressEnabled = enableIngress;

    if (modelName == "Default") {
        s_model = std::make_unique<DefaultDelayModel>();
    } else if (modelName == "CacheMiss") {
        s_model = std::make_unique<CacheMissDelayModel>();
    } else if (modelName == "UHN") {
        s_model = std::make_unique<UHNDelayModel>();
    } else {
        s_model = std::make_unique<DefaultDelayModel>();
    }

    s_model->Initialize(config, seed);

    std::cout << "DelayHooks initialized:" << "\n";
    std::cout << "  Model: " << (s_model ? s_model->GetName() : "none") << "\n";
    std::cout << "  Config: " << (config) << "\n";
    std::cout << "  Egress enabled: " << (enableEgress ? "yes" : "no") << "\n";
    std::cout << "  Ingress enabled: " << (enableIngress ? "yes" : "no") << "\n";
    std::cout << "  Seed: " << seed << "\n";

}

Time DelayHooks::DelayEgress(uint32_t nodeId, uint32_t bytes, uint32_t seq)
{
    if (!s_egressEnabled || !s_model)
    {
        return Time(0);
    }
    return s_model->CalculateEgressDelay(nodeId, bytes, seq);
}

Time DelayHooks::DelayIngress(uint32_t nodeId, uint32_t bytes, uint32_t seq)
{
    if (!s_ingressEnabled || !s_model)
    {
        return Time(0);
    }
    return s_model->CalculateIngressDelay(nodeId, bytes, seq);
}

bool DelayHooks::IsEgressEnabled()
{
    return s_egressEnabled;
}

bool DelayHooks::IsIngressEnabled()
{
    return s_ingressEnabled;
}

DelayModel* DelayHooks::GetActiveModel()
{
    return s_model.get();
}

void DelayHooks::SetNodeProperties(uint32_t nodeId, const NodeProperties& props)
{
    if (s_model)
    {
        return s_model->SetNodeProperties(nodeId, props);
    }

    std::cout << "DelayHook has no model!" << std::endl;
}

NodeProperties DelayHooks::GetNodeProperties(uint32_t nodeId)
{
    if (s_model)
    {
        return s_model->GetNodeProperties(nodeId);
    }
    
    std::cout << "DelayHook has no model!" << std::endl;
    return NodeProperties();
}

} // namespace ns3
