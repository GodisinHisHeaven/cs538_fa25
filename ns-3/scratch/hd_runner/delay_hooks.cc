/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * CS538 Host Delay Hooks - No-op Implementation
 *
 * This file provides no-op implementations of delay hooks.
 * All hooks return zero delay and log invocations for traceability.
 */

#include "delay_hooks.h"
#include "ns3/log.h"
#include <iostream>
namespace ns3 {

NS_LOG_COMPONENT_DEFINE("DelayHooks");

// Latency data in nanoseconds
static const uint32_t C2M[] = {30, 70, 105, 135, 160, 205};
static const uint32_t P2M[] = {30, 70, 110, 200, 240, 270};
static const size_t LATENCY_ARRAY_SIZE = 6;

/**
 * Calculate total latency = 2 * P2M[i-1] + 2 * C2M[i-1]
 * where i is the number of C2M cores (1 to 6)
 */
uint32_t CalculateTotalLatency(uint32_t numCores)
{
    if (numCores < 1 || numCores > LATENCY_ARRAY_SIZE)
    {
        NS_LOG_ERROR("Invalid number of cores: " << numCores
                     << " (must be between 1 and 6)");
        return 0;
    }

    uint32_t index = numCores - 1;
    uint32_t totalLatency = 2 * P2M[index] + 2 * C2M[index];

    std::cout << "Latency calculation for " << numCores << " C2M cores: "
                 << "2*P2M[" << index << "] + 2*C2M[" << index << "] = "
                 << "2*" << P2M[index] << " + 2*" << C2M[index] << " = "
                 << totalLatency << " ns" << std::endl;

    return totalLatency;
}

// Static member initialization
bool DelayHooks::s_egressEnabled = false;
bool DelayHooks::s_ingressEnabled = false;
std::string DelayHooks::s_configPath = "";
uint32_t DelayHooks::s_seed = 0;

void DelayHooks::Initialize(const std::string& configPath,
                       bool enableEgress,
                       bool enableIngress,
                       uint32_t seed)
{
    s_configPath = configPath;
    s_egressEnabled = enableEgress;
    s_ingressEnabled = enableIngress;
    s_seed = seed;

    NS_LOG_INFO("DelayHooks initialized:");
    NS_LOG_INFO("  Egress enabled: " << (enableEgress ? "yes" : "no"));
    NS_LOG_INFO("  Ingress enabled: " << (enableIngress ? "yes" : "no"));
    NS_LOG_INFO("  Config path: " << (configPath.empty() ? "(none)" : configPath));
    NS_LOG_INFO("  Seed: " << seed);
    NS_LOG_INFO("  Current behavior: Ingress uses C2M core 6 latency");
}

Time DelayHooks::DelayEgress(uint32_t nodeId, uint32_t bytes, uint32_t seq)
{
    if (!s_egressEnabled)
    {
        return Time(0);
    }

    NS_LOG_DEBUG("DelayEgress called: node=" << nodeId
                 << " bytes=" << bytes
                 << " seq=" << seq);

    // NO-OP: Return zero delay
    // Future implementation will calculate delay based on s_configPath
    return Time(0);
}

Time DelayHooks::DelayIngress(uint32_t nodeId, uint32_t bytes, uint32_t seq)
{
    if (!s_ingressEnabled)
    {
        return Time(0);
    }

    NS_LOG_DEBUG("DelayIngress called: node=" << nodeId
                 << " bytes=" << bytes
                 << " seq=" << seq);

    // Calculate latency for C2M core 6
    uint32_t latencyNs = CalculateTotalLatency(6);

    std::cout << "Adding ingress latency: " << latencyNs << " ns" << std::endl;

    return NanoSeconds(latencyNs);
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
