/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * CS538 Host Delay Hooks - Interface
 *
 * This file defines no-op hook interfaces for host-delay modeling.
 * These hooks are called at egress (before NIC Tx) and ingress (before app delivery).
 *
 * Current implementation: No-op (returns zero delay)
 * Future: Model team will implement actual delay logic via --hookConfigPath
 */

#ifndef DELAY_HOOKS_H
#define DELAY_HOOKS_H

#include "ns3/nstime.h"
#include <cstdint>

namespace ns3 {

/**
 * @brief Host delay hooks for egress and ingress packet processing
 *
 * These hooks provide insertion points for host-delay modeling without
 * requiring changes to the experiment harness.
 */
class DelayHooks
{
public:
    /**
     * @brief Initialize the delay hooks with configuration
     * @param configPath Path to model configuration file (currently ignored)
     * @param enableEgress Enable egress hook
     * @param enableIngress Enable ingress hook
     * @param seed Random seed for deterministic behavior
     */
    static void Initialize(const std::string& configPath,
                          bool enableEgress,
                          bool enableIngress,
                          uint32_t seed);

    /**
     * @brief Egress hook - called immediately before handing packet to L2/NIC
     * @param nodeId Node identifier
     * @param bytes Packet size in bytes
     * @param seq Sequence number for tracking
     * @return Delay to apply (currently returns zero)
     */
    static Time DelayEgress(uint32_t nodeId, uint32_t bytes, uint32_t seq);

    /**
     * @brief Ingress hook - called immediately before delivering to application
     * @param nodeId Node identifier
     * @param bytes Packet size in bytes
     * @param seq Sequence number for tracking
     * @return Delay to apply (currently returns zero)
     */
    static Time DelayIngress(uint32_t nodeId, uint32_t bytes, uint32_t seq);

    /**
     * @brief Check if egress hook is enabled
     */
    static bool IsEgressEnabled();

    /**
     * @brief Check if ingress hook is enabled
     */
    static bool IsIngressEnabled();

private:
    static bool s_egressEnabled;
    static bool s_ingressEnabled;
    static std::string s_configPath;
    static uint32_t s_seed;
};

} // namespace ns3

#endif /* DELAY_HOOKS_H */
