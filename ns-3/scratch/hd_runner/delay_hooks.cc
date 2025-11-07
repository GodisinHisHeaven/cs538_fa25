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
// Configuration Structure
// ============================================================================

struct DelayModelConfig
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

// Default configuration profiles
static DelayModelConfig CreateRealisticConfig()
{
    DelayModelConfig config;
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

static DelayModelConfig CreateModerateConfig()
{
    DelayModelConfig config = CreateRealisticConfig();
    config.severityMultiplier = 3.0;  // 3x impact
    return config;
}

static DelayModelConfig CreateSevereConfig()
{
    DelayModelConfig config = CreateRealisticConfig();
    config.severityMultiplier = 10.0;  // 10x impact
    return config;
}

// ============================================================================
// Static State
// ============================================================================

// Per-node timestamp tracking for load calculation
static std::map<uint32_t, std::deque<int64_t>> g_nodeTimestamps;

// Global configuration
static DelayModelConfig g_config;
static bool g_configInitialized = false;

// Static member initialization
bool DelayHooks::s_egressEnabled = false;
bool DelayHooks::s_ingressEnabled = false;
std::string DelayHooks::s_configPath = "";
uint32_t DelayHooks::s_seed = 0;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Calculate cache misses probabilistically based on packet size
 *
 * Small packets (<= 256B): Low miss probability (0.1)
 * Medium packets (256B-1KB): Medium miss probability (0.4)
 * Large packets (> 1KB): High miss probability (0.7)
 *
 * Returns estimated number of cache line misses
 */
static uint32_t CalculateCacheMisses(uint32_t bytes)
{
    if (!g_configInitialized)
    {
        return 0;
    }

    double missProb;
    if (bytes <= g_config.cache.smallPacketThreshold)
    {
        missProb = g_config.cache.missProbabilitySmall;
    }
    else if (bytes <= g_config.cache.largePacketThreshold)
    {
        missProb = g_config.cache.missProbabilityMedium;
    }
    else
    {
        missProb = g_config.cache.missProbabilityLarge;
    }

    // Calculate number of cache lines this packet spans
    uint32_t cacheLines = (bytes + g_config.cache.cacheLineSize - 1) /
                          g_config.cache.cacheLineSize;

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
static double CalculateLoad(uint32_t nodeId, int64_t currentTimeNs)
{
    if (!g_configInitialized)
    {
        return 0.0;
    }

    auto& timestamps = g_nodeTimestamps[nodeId];

    // Remove expired timestamps (older than window)
    int64_t windowNs = static_cast<int64_t>(g_config.loadTracking.windowSizeMs) * 1000000LL;
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
    double load = actualRate / g_config.loadTracking.nominalRatePps;

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
static uint32_t CalculatePenalty(double load)
{
    if (!g_configInitialized)
    {
        return 0;
    }

    uint32_t penalty = g_config.penalty.basePenaltyNs;

    if (load >= g_config.penalty.loadThreshold)
    {
        penalty = static_cast<uint32_t>(penalty * g_config.penalty.highLoadMultiplier);
        NS_LOG_DEBUG("CalculatePenalty: HIGH LOAD - load=" << load
                     << " threshold=" << g_config.penalty.loadThreshold
                     << " penalty=" << penalty);
    }
    else
    {
        NS_LOG_DEBUG("CalculatePenalty: normal load - load=" << load
                     << " penalty=" << penalty);
    }

    return static_cast<uint32_t>(penalty * g_config.severityMultiplier);
}

/**
 * Calculate queueing delay based on load (step function with growth)
 *
 * Below threshold: Base queue delay
 * Above threshold: Base delay + growth based on excess load
 *
 * Returns queueing delay in nanoseconds
 */
static uint32_t CalculateQueueDelay(double load)
{
    if (!g_configInitialized)
    {
        return 0;
    }

    uint32_t queueDelay = g_config.queue.baseQueueNs;

    if (load >= g_config.queue.loadThreshold)
    {
        double excessLoad = load - g_config.queue.loadThreshold;
        uint32_t additionalDelay = static_cast<uint32_t>(
            g_config.queue.baseQueueNs * g_config.queue.queueGrowthFactor * excessLoad
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

    return static_cast<uint32_t>(queueDelay * g_config.severityMultiplier);
}

void DelayHooks::Initialize(const std::string& configPath,
                       bool enableEgress,
                       bool enableIngress,
                       uint32_t seed)
{
    s_configPath = configPath;
    s_egressEnabled = enableEgress;
    s_ingressEnabled = enableIngress;
    s_seed = seed;

    // Load configuration based on config path
    // For now, use simple profile selection based on configPath string
    if (configPath.empty() || configPath == "realistic")
    {
        g_config = CreateRealisticConfig();
        NS_LOG_INFO("  Using REALISTIC config (severity=1.0, ~2us impact)");
    }
    else if (configPath == "moderate")
    {
        g_config = CreateModerateConfig();
        NS_LOG_INFO("  Using MODERATE config (severity=3.0, ~6us impact)");
    }
    else if (configPath == "severe")
    {
        g_config = CreateSevereConfig();
        NS_LOG_INFO("  Using SEVERE config (severity=10.0, ~20us impact)");
    }
    else
    {
        // Default to realistic
        g_config = CreateRealisticConfig();
        NS_LOG_INFO("  Unknown config '" << configPath << "', using REALISTIC");
    }

    g_configInitialized = true;

    // Clear any existing timestamp data
    g_nodeTimestamps.clear();

    NS_LOG_INFO("DelayHooks initialized:");
    NS_LOG_INFO("  Egress enabled: " << (enableEgress ? "yes" : "no"));
    NS_LOG_INFO("  Ingress enabled: " << (enableIngress ? "yes" : "no"));
    NS_LOG_INFO("  Config: " << (configPath.empty() ? "realistic (default)" : configPath));
    NS_LOG_INFO("  Seed: " << seed);
    NS_LOG_INFO("  Model: base=" << g_config.baseCpuCyclesNs << "ns"
                << " severity=" << g_config.severityMultiplier
                << " penalty_threshold=" << g_config.penalty.loadThreshold
                << " queue_threshold=" << g_config.queue.loadThreshold);
}

Time DelayHooks::DelayEgress(uint32_t nodeId, uint32_t bytes, uint32_t seq)
{
    if (!s_egressEnabled || !g_configInitialized)
    {
        return Time(0);
    }

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
    uint32_t totalDelayNs = g_config.baseCpuCyclesNs +
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

    return NanoSeconds(totalDelayNs);
}

Time DelayHooks::DelayIngress(uint32_t nodeId, uint32_t bytes, uint32_t seq)
{
    if (!s_ingressEnabled || !g_configInitialized)
    {
        return Time(0);
    }

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
    uint32_t totalDelayNs = g_config.baseCpuCyclesNs +
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
    static uint32_t packetCount = 0;
    packetCount++;
    if (packetCount % 1000 == 0)
    {
        std::cout << "Ingress delay (pkt " << packetCount << "): "
                  << "load=" << load
                  << " misses=" << cacheMisses
                  << " delay=" << totalDelayNs << "ns" << std::endl;
    }

    return NanoSeconds(totalDelayNs);
}

bool DelayHooks::IsEgressEnabled()
{
    return s_egressEnabled;
}

bool DelayHooks::IsIngressEnabled()
{
    return s_ingressEnabled;
}

} // namespace ns3
