/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * CS538 Host Delay Hooks - No-op Implementation
 *
 * This file provides no-op implementations of delay hooks.
 * All hooks return zero delay and log invocations for traceability.
 */

#include "delay_hooks.h"
#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("DelayHooks");

// Static member initialization
bool DelayHooks::s_egressEnabled = false;
bool DelayHooks::s_ingressEnabled = false;
std::string DelayHooks::s_configPath = "";
uint32_t DelayHooks::s_seed = 0;

void
DelayHooks::Initialize(const std::string& configPath,
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
    NS_LOG_INFO("  Current behavior: NO-OP (zero delay)");
}

Time
DelayHooks::DelayEgress(uint32_t nodeId, uint32_t bytes, uint32_t seq)
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

Time
DelayHooks::DelayIngress(uint32_t nodeId, uint32_t bytes, uint32_t seq)
{
    if (!s_ingressEnabled)
    {
        return Time(0);
    }

    NS_LOG_DEBUG("DelayIngress called: node=" << nodeId
                 << " bytes=" << bytes
                 << " seq=" << seq);

    // NO-OP: Return zero delay
    // Future implementation will calculate delay based on s_configPath
    return Time(0);
}

bool
DelayHooks::IsEgressEnabled()
{
    return s_egressEnabled;
}

bool
DelayHooks::IsIngressEnabled()
{
    return s_ingressEnabled;
}

} // namespace ns3
