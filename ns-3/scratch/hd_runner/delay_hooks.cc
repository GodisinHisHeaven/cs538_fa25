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

    Time CalculateEgressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq) override
    {
        return Time(0);
    }
    Time CalculateIngressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq) override
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

    Time CalculateEgressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq) override
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

    Time CalculateIngressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq) override
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


class AnalyticalDelayModel: public DelayModel
{

private:
    std::mt19937 m_rng;
    bool m_enableRandomness = false;
    double m_randomnessFactor = 0.2; // Fixed 20% variation

    void ParseConfig(const std::string& config)
    {
        if (config == "true") {
            m_enableRandomness = true;
            m_randomnessFactor = 0.1; // Default 10% variation
        } else {
            m_enableRandomness = false;
            m_randomnessFactor = 0.0;
        }
        std::cout << "AnalyticalDelayModel config: enableRandomness=" << m_enableRandomness
                  << " randomnessFactor=" << m_randomnessFactor << std::endl;
    }

    int64_t ApplyRandomness(int64_t baseValue)
    {
        if (!m_enableRandomness || m_randomnessFactor <= 0.0) {
            return baseValue;
        }

        // Normal distribution: mean=0, std_dev=randomnessFactor/3
        // This gives ~99.7% of values within randomnessFactor
        std::normal_distribution<double> dist(0.0, m_randomnessFactor / 3.0);
        double variation = dist(m_rng);

        // Apply variation to base value
        int64_t randomValue = static_cast<int64_t>(baseValue * (1.0 + variation));

        // Ensure non-negative result
        return std::max(int64_t(0), randomValue);
    }

public:
    std::string GetName() const override
    {
        return "Analytical";
    }

    void Initialize(const std::string& config, uint32_t seed) override {
        m_rng.seed(seed);
        ParseConfig(config);
    }

    void Reset() override {}

    Time CalculateEgressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq) override
    {
        return Time(0);
    }
    Time CalculateIngressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq) override
    {
        NodeProperties props = GetNodeProperties(nodeId);

        // uint32_t p2mWriteConstantLatency = 300;

        // uint32_t switchingDelay = static_cast<uint32_t>(14 * 0.50 * 8); //averageRpqOccupancy * numSwitches  / linesRead * timeToSwitchWriteToRead;
        // uint32_t writeHolBlocking = static_cast<uint32_t>(14 * 0.50 * 2); // averageRpqOccupancy * linesWritten / linesRead * timeToTransmit;
        // uint32_t readHolBlocking = static_cast<uint32_t>((14 - 1) * 2); // (averageRpqOccupancy - 1) * timeToTransmit;
        // uint32_t topOfQueueDelay = static_cast<uint32_t>(0.25 * 14 + 0.25 * 14);// numActRead / linesRead * timeToAct + numPreConflictRead / linesRead * timeToPre;

        // uint32_t lRead = constantRead + switchingDelay + writeHolBlocking + readHolBlocking + topOfQueueDelay;

        // //ReadWrite caused by reading to cache during a write and writing back later on eviction
        // // P2M write uses this
        // // C2M-Write Domain: LFB -> CHA (credit allocated at LFB and replenished at CHA)
        // // - Degradation in tput caused by DRAM row miss ratio and load imbalance across banks
        // // P2M-Write Domain: IIO -> CHA -> MC (credit allocated at IIO and replenished at MC)
        // // - latency inflation in P2M-Write Domain latency (~20-30ns)
        // // - with 3-4 cores C2M start to reduce P2M tput as memory bandwidth gets saurated (MC write queue full) leading to backlog of writes at CHA
        // // - domain inflation only for P2M-write domain (3-4 C2M cores result in latency increase 1.5x) and all domain credits used (92)
        // // C2M ReadWrite bound by C2M Read domain latency (~12% increase) as C2M write domain latency does not increase
        // // As write backlog at CHAT continue to increas with >4C2M cores, CHA begins to apply backpressure (RPQ capped impacting both domains 50ns)

        // // throughput = avg IIO ocupancy / avg latency
        // // For Nwaiting, we use counters from the CHA since this is where backlog would build up
        // uint32_t switchingDelay = static_cast<uint32_t>(14 * 0.50 * 8); // numWriteRequestsWaiting * numSwitches  / linesWritten * timeToSwitchReadToWrite;
        // uint32_t readHolBlocking = static_cast<uint32_t>(14 * 0.50 * 2); //numWriteRequestsWaiting * linesRead / linesWritten * timeToTransmit;
        // uint32_t writeHolBlocking = static_cast<uint32_t>((14 - 1)  * 2);// (numWriteRequestsWaiting - 1) * timeToTransmit;
        // uint32_t topOfQueueDelay = static_cast<uint32_t>(0.25 * 14 + 0.25 * 14);//numActWrite / linesWritten * timeToAct + numPreConflictWrite / linesWritten * timeToPre;

        // uint32_t lWrite = constantWrite + probabilityWpqFull * (switchingDelay + readHolBlocking + writeHolBlocking + topOfQueueDelay);

        std::map<int, int> p2mLatency = {
            {0, 0},
            {1, 40},
            {2, 60},
            {3, 130},
            {4, 165}
        };

        if (props.cpuCoreContention > 4 || props.cpuCoreContention < 0) {
            std::cout << "Invalid cpuCoreContention value: " << props.cpuCoreContention << "\n";
            std::cout << "Valid range is 0-4. Returning 0 delay.\n";
            return NanoSeconds(0);
        }

        int64_t baseLatency = p2mLatency[props.cpuCoreContention];
        int64_t finalLatency = ApplyRandomness(baseLatency);

        return NanoSeconds(finalLatency);
    }

};

// ============================================================================
// Host Network Delay Model
// Based on "Understanding the Host Network" (SIGCOMM 2024)
// Models credit-based flow control across three domains:
//   - C2M-Write: LFB -> CHA (12 credits, replenished at CHA)
//   - C2M-Read:  LFB -> CHA -> MC (12 credits, replenished at MC)
//   - P2M-Write: IIO -> CHA -> MC (92 credits, replenished at MC)
// ============================================================================

/**
 * Represents an in-flight request with credit tracking
 */
struct InFlightRequest
{
    int64_t sendTimeNs;       // When the request was sent
    int64_t completionTimeNs; // When credit will be replenished
    uint32_t bytes;           // Request size
    uint32_t creditsUsed;     // Number of credits consumed by this request
};

/**
 * Credit-based queue model for a single domain
 */
class CreditQueue
{
public:
    CreditQueue(const std::string& name, uint32_t maxCredits, int64_t baseLatencyNs)
        : m_name(name),
          m_maxCredits(maxCredits),
          m_availableCredits(maxCredits),
          m_baseLatencyNs(baseLatencyNs)
    {
    }

    /**
     * Try to send a request. Returns the queueing delay if credits need to wait.
     * @param nowNs Current simulation time in nanoseconds
     * @param latencyNs Latency until credit is replenished (domain-specific)
     * @param creditsNeeded Number of credits needed (typically bytes/64 for cache lines)
     * @return Queueing delay in nanoseconds (0 if credits immediately available)
     *
     * Note: If creditsNeeded > maxCredits, the request is split into multiple
     * sub-requests, each using at most maxCredits. This models how large packets
     * must be transferred in multiple batches through the credit-based pipeline.
     */
    int64_t SendRequest(int64_t nowNs, int64_t latencyNs, uint32_t creditsNeeded = 1)
    {
        // First, replenish any credits from completed requests
        ReplenishCredits(nowNs);

        int64_t totalQueueDelay = 0;
        uint32_t remainingCredits = creditsNeeded;

        // Process in batches if creditsNeeded exceeds maxCredits
        while (remainingCredits > 0)
        {
            // Determine how many credits to use in this batch (at most maxCredits)
            uint32_t batchCredits = std::min(remainingCredits, m_maxCredits);

            // Wait until we have enough credits for this batch
            while (m_availableCredits < batchCredits)
            {
                // Not enough credits - must wait for requests to complete
                if (!m_inFlightRequests.empty())
                {
                    int64_t earliestCompletion = m_inFlightRequests.front().completionTimeNs;
                    int64_t waitTime = std::max(int64_t(0), earliestCompletion - nowNs);
                    totalQueueDelay += waitTime;
                    
                    // Update nowNs to when we actually can proceed (after waiting)
                    nowNs += waitTime;
                    
                    // Replenish credits at the new time
                    ReplenishCredits(nowNs);
                }
                else
                {
                    // No in-flight requests but not enough credits - shouldn't happen normally
                    // Reset credits to max to avoid infinite loop
                    m_availableCredits = m_maxCredits;
                    break;
                }
            }

            // Consume the credits for this batch
            uint32_t creditsToUse = std::min(batchCredits, m_availableCredits);
            m_availableCredits -= creditsToUse;

            // Track this batch as an in-flight request
            InFlightRequest req;
            req.sendTimeNs = nowNs;
            req.completionTimeNs = nowNs + latencyNs;
            req.bytes = creditsToUse * 64; // Track bytes for reference
            req.creditsUsed = creditsToUse;
            m_inFlightRequests.push_back(req);

            remainingCredits -= creditsToUse;
        }

        return totalQueueDelay;
    }

    /**
     * Get current credit utilization (0.0 - 1.0+)
     */
    double GetUtilization() const
    {
        uint32_t inFlight = m_maxCredits - m_availableCredits;
        return static_cast<double>(inFlight) / static_cast<double>(m_maxCredits);
    }

    /**
     * Get number of requests waiting (queue depth)
     */
    uint32_t GetQueueDepth() const
    {
        return static_cast<uint32_t>(m_inFlightRequests.size());
    }

    /**
     * Reset the queue state
     */
    void Reset()
    {
        m_availableCredits = m_maxCredits;
        m_inFlightRequests.clear();
    }

    std::string GetName() const { return m_name; }
    uint32_t GetMaxCredits() const { return m_maxCredits; }
    uint32_t GetAvailableCredits() const { return m_availableCredits; }

private:
    void ReplenishCredits(int64_t nowNs)
    {
        while (!m_inFlightRequests.empty() &&
               m_inFlightRequests.front().completionTimeNs <= nowNs)
        {
            uint32_t creditsToReturn = m_inFlightRequests.front().creditsUsed;
            m_inFlightRequests.pop_front();
            m_availableCredits = std::min(m_availableCredits + creditsToReturn, m_maxCredits);
        }
    }

    std::string m_name;
    uint32_t m_maxCredits;
    uint32_t m_availableCredits;
    int64_t m_baseLatencyNs;
    std::deque<InFlightRequest> m_inFlightRequests;
};

/**
 * Configuration for HostNetwork delay model
 */
struct HostNetworkConfig
{
    // C2M-Write Domain: LFB -> CHA
    struct {
        uint32_t maxCredits = 12;
        int64_t baseLatencyNs = 10;  // ~10ns LFB to CHA
    } c2mWrite;

    // C2M-Read Domain: LFB -> CHA -> MC
    struct {
        uint32_t maxCredits = 12;
        int64_t baseLatencyNs = 70;  // ~70ns LFB to MC (including CHA)
    } c2mRead;

    // P2M-Write Domain: IIO -> CHA -> MC
    struct {
        uint32_t maxCredits = 92;
        int64_t baseLatencyNs = 300;  // ~300ns IIO to MC
    } p2mWrite;

    // Contention multipliers
    struct {
        double c2mContentionFactor = 1.2;   // 12% increase under C2M load
        double p2mContentionFactor = 1.5;   // 50% increase with 3-4 C2M cores
        double backpressureThreshold = 0.9; // CHA backpressure kicks in at 90% utilization
        int64_t backpressurePenaltyNs = 50; // Additional 50ns when CHA applies backpressure
    } contention;

    // Randomness for realistic variance
    bool enableRandomness = true;
    double randomnessFactor = 0.2; // 20% variation
};

class HostNetworkDelayModel : public DelayModel
{
public:
    HostNetworkDelayModel()
        : m_c2mWriteQueue("C2M-Write", 12, 10),
          m_c2mReadQueue("C2M-Read", 12, 70),
          m_p2mWriteQueue("P2M-Write", 92, 300)
    {
    }

    std::string GetName() const override
    {
        return "HostNetwork";
    }

    void Initialize(const std::string& config, uint32_t seed) override
    {
        m_rng.seed(seed);
        ParseConfig(config);

        std::cout << "HostNetworkDelayModel initialized:" << std::endl;
        std::cout << "  C2M-Write: " << m_config.c2mWrite.maxCredits << " credits, "
                  << m_config.c2mWrite.baseLatencyNs << "ns base latency" << std::endl;
        std::cout << "  C2M-Read:  " << m_config.c2mRead.maxCredits << " credits, "
                  << m_config.c2mRead.baseLatencyNs << "ns base latency" << std::endl;
        std::cout << "  P2M-Write: " << m_config.p2mWrite.maxCredits << " credits, "
                  << m_config.p2mWrite.baseLatencyNs << "ns base latency" << std::endl;
        std::cout << "  Randomness: " << (m_config.enableRandomness ? "enabled" : "disabled")
                  << " (" << (m_config.randomnessFactor * 100) << "%)" << std::endl;
    }

    void Reset() override
    {
        m_c2mWriteQueue.Reset();
        m_c2mReadQueue.Reset();
        m_p2mWriteQueue.Reset();
    }

    Time CalculateEgressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq) override
    {
        return Time(0);
    }

    Time CalculateIngressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq) override
    {
        int64_t nowNs = Simulator::Now().GetNanoSeconds();
        NodeProperties props = GetNodeProperties(nodeId);

        if (props.cpuCoreContention <= 0) {
            return NanoSeconds(0);
        }

        int64_t totalDelayNs = 0;

        // Calculate credits needed based on cache lines (bytes / 64)
        uint32_t cacheLines = (bytes + 63) / 64; // Round up to nearest cache line

        // Step 1: P2M-Write - NIC DMAs packet to memory
        int64_t p2mWriteLatency = CalculateP2MWriteLatency(props);
        int64_t p2mWriteQueueDelay = m_p2mWriteQueue.SendRequest(nowNs, p2mWriteLatency, cacheLines);
        totalDelayNs += p2mWriteQueueDelay + p2mWriteLatency;

        // Step 2: C2M-Read - CPU reads packet from memory
        int64_t c2mReadLatency = CalculateC2MReadLatency(props);
        int64_t c2mReadQueueDelay = m_c2mReadQueue.SendRequest(nowNs + totalDelayNs, c2mReadLatency, cacheLines);
        totalDelayNs += c2mReadQueueDelay + c2mReadLatency;

        // Apply contention effects
        totalDelayNs = ApplyContentionEffects(totalDelayNs, props);

        // Apply randomness
        totalDelayNs = ApplyRandomness(totalDelayNs);

        NS_LOG_DEBUG("HostNetwork Ingress: node=" << nodeId
                    << " bytes=" << bytes
                    << " p2mWriteDelay=" << p2mWriteQueueDelay
                    << " c2mReadDelay=" << c2mReadQueueDelay
                    << " totalDelay=" << totalDelayNs << "ns");

        return NanoSeconds(totalDelayNs);
    }

private:
    HostNetworkConfig m_config;
    std::mt19937 m_rng;

    // Credit queues for each domain
    CreditQueue m_c2mWriteQueue;
    CreditQueue m_c2mReadQueue;
    CreditQueue m_p2mWriteQueue;

    void ParseConfig(const std::string& config)
    {
        if (config.empty() || config == "default")
        {
            // Use default configuration
        }
        else if (config == "high_contention")
        {
            m_config.contention.c2mContentionFactor = 1.5;
            m_config.contention.p2mContentionFactor = 2.0;
            m_config.contention.backpressurePenaltyNs = 100;
        }
        else if (config == "no_random")
        {
            m_config.enableRandomness = false;
        }

        // Re-initialize queues with config values
        m_c2mWriteQueue = CreditQueue("C2M-Write",
                                       m_config.c2mWrite.maxCredits,
                                       m_config.c2mWrite.baseLatencyNs);
        m_c2mReadQueue = CreditQueue("C2M-Read",
                                      m_config.c2mRead.maxCredits,
                                      m_config.c2mRead.baseLatencyNs);
        m_p2mWriteQueue = CreditQueue("P2M-Write",
                                       m_config.p2mWrite.maxCredits,
                                       m_config.p2mWrite.baseLatencyNs);
    }

    /**
     * Calculate C2M-Write domain latency
     * LFB -> CHA path
     */
    int64_t CalculateC2MWriteLatency(const NodeProperties& props)
    {
        int64_t latency = m_config.c2mWrite.baseLatencyNs;

        // C2M-Write latency is relatively stable but increases slightly with contention
        if (props.cpuCoreContention > 0)
        {
            // Minimal impact on C2M-Write domain
            double factor = 1.0 + (props.cpuCoreContention * 0.05);
            latency = static_cast<int64_t>(latency * factor);
        }

        return latency;
    }

    /**
     * Calculate C2M-Read domain latency
     * LFB -> CHA -> MC path
     */
    int64_t CalculateC2MReadLatency(const NodeProperties& props)
    {
        int64_t latency = m_config.c2mRead.baseLatencyNs;

        if (props.cpuCoreContention > 0)
        {
            latency = static_cast<int64_t>(latency * m_config.contention.c2mContentionFactor);
        }

        return latency;
    }

    /**
     * Calculate P2M-Write domain latency
     * IIO -> CHA -> MC path
     */
    int64_t CalculateP2MWriteLatency(const NodeProperties& props)
    {
        int64_t latency = m_config.p2mWrite.baseLatencyNs;

        // P2M-Write shows significant latency inflation with C2M contention
        if (props.cpuCoreContention >= 3)
        {
            latency = static_cast<int64_t>(latency * m_config.contention.p2mContentionFactor);
        }
        else if (props.cpuCoreContention > 0)
        {
            // Gradual increase for lower contention
            double factor = 1.0 + (props.cpuCoreContention * 0.1);
            latency = static_cast<int64_t>(latency * factor);
        }

        return latency;
    }

    /**
     * Apply contention effects based on queue utilization
     * Models CHA backpressure when queues are heavily utilized
     */
    int64_t ApplyContentionEffects(int64_t baseDelayNs, const NodeProperties& props)
    {
        int64_t delay = baseDelayNs;

        // Check for CHA backpressure (high utilization across queues)
        double maxUtilization = std::max({
            m_c2mWriteQueue.GetUtilization(),
            m_c2mReadQueue.GetUtilization(),
            m_p2mWriteQueue.GetUtilization()
        });

        if (maxUtilization >= m_config.contention.backpressureThreshold)
        {
            // CHA applies backpressure, adding additional delay
            delay += m_config.contention.backpressurePenaltyNs;
            NS_LOG_DEBUG("CHA backpressure applied: utilization=" << maxUtilization
                        << " penalty=" << m_config.contention.backpressurePenaltyNs << "ns");
        }

        return delay;
    }

    /**
     * Apply random variance for realistic behavior
     */
    int64_t ApplyRandomness(int64_t baseValue)
    {
        if (!m_config.enableRandomness || m_config.randomnessFactor <= 0.0)
        {
            return baseValue;
        }

        std::normal_distribution<double> dist(0.0, m_config.randomnessFactor / 3.0);
        double variation = dist(m_rng);

        int64_t randomValue = static_cast<int64_t>(baseValue * (1.0 + variation));
        return std::max(int64_t(0), randomValue);
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
    } else if (modelName == "Analytical") {
        s_model = std::make_unique<AnalyticalDelayModel>();
    } else if (modelName == "HostNetwork") {
        s_model = std::make_unique<HostNetworkDelayModel>();
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
