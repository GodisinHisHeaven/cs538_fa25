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
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/random-variable-stream.h"
#include "ns3/simulator.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

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

    // Ingress-only additive delay to better match real hosts
    struct {
        uint32_t baseIngressNs;
        uint32_t tailSlopeNs; // multiplied by load (capped) for tail lift
    } ingress;

    // Load tracking
    struct {
        uint32_t windowSizeMs;
        double nominalRatePps;
        double maxLoad;
        double alpha;
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
    config.cache.missProbabilitySmall = 0.02;
    config.cache.missProbabilityMedium = 0.05;
    config.cache.missProbabilityLarge = 0.10;

    config.penalty.basePenaltyNs = 20;
    config.penalty.loadThreshold = 0.7;
    config.penalty.highLoadMultiplier = 3.0;

    config.queue.baseQueueNs = 20;
    config.queue.loadThreshold = 0.6;
    config.queue.queueGrowthFactor = 1.0;

    config.ingress.baseIngressNs = 50000; // ~50us baseline host stack cost
    config.ingress.tailSlopeNs = 0;       // disable tail for stability during tuning

    config.loadTracking.windowSizeMs = 10;
    config.loadTracking.nominalRatePps = 3000000.0; // ~100 Gbps at 4K MTU
    config.loadTracking.maxLoad = 3.0;              // clamp load to avoid runaway
    config.loadTracking.alpha = 0.05;               // smoother EWMA

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

struct LoadState
{
    double ewmaLoad = 0.0;
    int64_t lastUpdateNs = 0;
};

// Per-node load tracking, split by direction
static std::map<uint32_t, LoadState> g_egressLoad;
static std::map<uint32_t, LoadState> g_ingressLoad;

// Global configuration
static DelayModelConfig g_config;
static bool g_configInitialized = false;

// Random variables
static Ptr<UniformRandomVariable> g_cacheMissRv;
static Ptr<NormalRandomVariable> g_baseJitterRv;

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

    // Sample cache misses probabilistically per cache line
    uint32_t cacheMisses = 0;
    for (uint32_t i = 0; i < cacheLines; i++)
    {
        if (g_cacheMissRv && g_cacheMissRv->GetValue() < missProb)
        {
            cacheMisses++;
        }
    }

    NS_LOG_DEBUG("CalculateCacheMisses: bytes=" << bytes
                 << " cacheLines=" << cacheLines
                 << " missProb=" << missProb
                 << " misses=" << cacheMisses);

    return cacheMisses;
}

/**
 * Calculate current load factor using an EWMA that decays with idle time
 *
 * Returns load factor (0.0 = no load, 1.0 = nominal load, >1.0 = overload)
 */
static double CalculateLoad(uint32_t nodeId, bool isEgress, int64_t currentTimeNs)
{
    if (!g_configInitialized)
    {
        return 0.0;
    }

    auto& state = isEgress ? g_egressLoad[nodeId] : g_ingressLoad[nodeId];

    // Decay existing load for long idle periods
    int64_t windowNs = static_cast<int64_t>(g_config.loadTracking.windowSizeMs) * 1000000LL;
    if (state.lastUpdateNs > 0 && currentTimeNs > state.lastUpdateNs)
    {
        int64_t deltaNs = currentTimeNs - state.lastUpdateNs;
        double decay = std::exp(-static_cast<double>(deltaNs) / windowNs);
        state.ewmaLoad *= decay;
    }

    // Compute instantaneous load from the gap since last packet
    double instLoad = 0.0;
    if (state.lastUpdateNs > 0 && currentTimeNs > state.lastUpdateNs)
    {
        double deltaNs = static_cast<double>(currentTimeNs - state.lastUpdateNs);
        double instRatePps = 1e9 / deltaNs;
        instLoad = instRatePps / g_config.loadTracking.nominalRatePps;
        instLoad = std::min(instLoad, g_config.loadTracking.maxLoad);
    }

    // EWMA update
    double alpha = g_config.loadTracking.alpha;
    state.ewmaLoad = (alpha * instLoad) + ((1.0 - alpha) * state.ewmaLoad);
    state.ewmaLoad = std::min(state.ewmaLoad, g_config.loadTracking.maxLoad);
    state.lastUpdateNs = currentTimeNs;

    NS_LOG_DEBUG("CalculateLoad: node=" << nodeId
                 << " dir=" << (isEgress ? "egress" : "ingress")
                 << " instLoad=" << instLoad
                 << " ewma=" << state.ewmaLoad);

    return state.ewmaLoad;
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

    // Smooth growth to avoid discontinuities
    double cappedLoad = std::min(load, g_config.loadTracking.maxLoad);
    double multiplier = 1.0 + 0.5 * cappedLoad;
    double penalty = static_cast<double>(g_config.penalty.basePenaltyNs) * multiplier;

    NS_LOG_DEBUG("CalculatePenalty: load=" << load
                 << " multiplier=" << multiplier
                 << " penalty=" << penalty);

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

    double queueDelay = g_config.queue.baseQueueNs;

    if (load >= g_config.queue.loadThreshold)
    {
        double excessLoad = load - g_config.queue.loadThreshold;
        double additionalDelay = g_config.queue.baseQueueNs * g_config.queue.queueGrowthFactor * excessLoad;
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

    // Clear load tracking and initialize RNGs
    g_egressLoad.clear();
    g_ingressLoad.clear();

    g_cacheMissRv = CreateObject<UniformRandomVariable>();
    g_baseJitterRv = CreateObject<NormalRandomVariable>();
    g_baseJitterRv->SetAttribute("Mean", DoubleValue(0.0));
    g_baseJitterRv->SetAttribute("Variance", DoubleValue(50.0 * 50.0));

    // Seed streams for repeatability across runs
    if (seed != 0)
    {
        g_cacheMissRv->SetStream(seed + 1);
        g_baseJitterRv->SetStream(seed + 2);
    }

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

    // Calculate load factor (egress only)
    double load = CalculateLoad(nodeId, true, nowNs);

    // Calculate cache misses
    uint32_t cacheMisses = CalculateCacheMisses(bytes);

    // Calculate penalty (load-dependent)
    uint32_t penaltyPerMiss = CalculatePenalty(load);

    // Calculate queueing delay (load-dependent)
    uint32_t queueDelay = CalculateQueueDelay(load);

    // Ingress shaping to mimic host stack latency (only on ingress path)
    double cappedLoad = std::min(load, g_config.loadTracking.maxLoad);
    uint32_t ingressShapingNs = g_config.ingress.baseIngressNs +
                                static_cast<uint32_t>(g_config.ingress.tailSlopeNs * cappedLoad);

    // Add base jitter (clamped to zero)
    uint32_t jitterNs = 0;
    if (g_baseJitterRv)
    {
        double jitter = g_baseJitterRv->GetValue();
        jitterNs = static_cast<uint32_t>(std::max<double>(0.0, std::lround(jitter)));
    }

    // Total delay = base + ingress shaping + jitter + (misses × penalty) + queue
    uint64_t totalDelayNs = static_cast<uint64_t>(g_config.baseCpuCyclesNs) +
                            static_cast<uint64_t>(ingressShapingNs) +
                            static_cast<uint64_t>(jitterNs) +
                            static_cast<uint64_t>(cacheMisses) * penaltyPerMiss +
                            static_cast<uint64_t>(queueDelay);

    // Cap to avoid pathological values
    totalDelayNs = std::min<uint64_t>(totalDelayNs, 1000000000ULL); // 1 second cap

    NS_LOG_DEBUG("DelayEgress: node=" << nodeId
                 << " bytes=" << bytes
                 << " seq=" << seq
                 << " load=" << load
                 << " misses=" << cacheMisses
                 << " penalty=" << penaltyPerMiss
                 << " queue=" << queueDelay
                 << " ingressShape=" << ingressShapingNs
                 << " jitter=" << jitterNs
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

    // Calculate load factor (ingress only)
    double load = CalculateLoad(nodeId, false, nowNs);

    // Calculate cache misses
    uint32_t cacheMisses = CalculateCacheMisses(bytes);

    // Calculate penalty (load-dependent)
    uint32_t penaltyPerMiss = CalculatePenalty(load);

    // Calculate queueing delay (load-dependent)
    uint32_t queueDelay = CalculateQueueDelay(load);

    // Add base jitter (clamped to zero)
    uint32_t jitterNs = 0;
    if (g_baseJitterRv)
    {
        double jitter = g_baseJitterRv->GetValue();
        jitterNs = static_cast<uint32_t>(std::max<double>(0.0, std::lround(jitter)));
    }

    // Total delay = base + jitter + (misses × penalty) + queue
    uint64_t totalDelayNs = static_cast<uint64_t>(g_config.baseCpuCyclesNs) +
                            static_cast<uint64_t>(jitterNs) +
                            static_cast<uint64_t>(cacheMisses) * penaltyPerMiss +
                            static_cast<uint64_t>(queueDelay);

    totalDelayNs = std::min<uint64_t>(totalDelayNs, 1000000000ULL); // 1 second cap

    NS_LOG_DEBUG("DelayIngress: node=" << nodeId
                 << " bytes=" << bytes
                 << " seq=" << seq
                 << " load=" << load
                 << " misses=" << cacheMisses
                 << " penalty=" << penaltyPerMiss
                 << " queue=" << queueDelay
                 << " jitter=" << jitterNs
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
