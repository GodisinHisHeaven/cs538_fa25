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
#include <map>
#include <memory>

namespace ns3 {

// Additional Node Properties that could be used in DelayHook
struct NodeProperties 
{
    uint32_t cpuCoreContention = 0; // Number of competing processes/threads

    NodeProperties() = default;
};

class DelayModel
{
public:
    virtual ~DelayModel() = default;
    virtual std::string GetName() const = 0;
    virtual void Initialize(const std::string& config, uint32_t seed) = 0;
    virtual void Reset() = 0;
    virtual Time CalculateEgressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq) = 0;
    virtual Time CalculateIngressDelay(uint32_t nodeId, uint32_t bytes, uint32_t seq) = 0;
    
    virtual void SetNodeProperties(uint32_t nodeId, const NodeProperties& props) 
    {
        nodeProperties[nodeId] = props;
    }

    virtual NodeProperties GetNodeProperties(uint32_t nodeId) const 
    {
        auto it = nodeProperties.find(nodeId);
        if (it != nodeProperties.end()) 
        {
            return it->second;
        } 
        return NodeProperties(); //Return default if not found
    }

protected:
    std::map<uint32_t, NodeProperties> nodeProperties;
};


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
     * @param config Model config
     * @param enableEgress Enable egress hook
     * @param enableIngress Enable ingress hook
     * @param seed Random seed for deterministic behavior
     */
    static void Initialize(const std::string& modelName,
                          const std::string& config,
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

    /**
     * @brief Get the current delay model (for testing/debugging)
     */
    static DelayModel* GetActiveModel();

    /**
     * @brief Set properties for a specific node
     * @param nodeId Node identifier
     * @param props Properties to associate with the node
     *
     * Example usage:
     *   DelayHooks::SetNodeProperties(0, NodeProperties{.cpuCoreContention = 4});
     */
    static void SetNodeProperties(uint32_t nodeId, const NodeProperties& props);

     /**
     * @brief Get properties for a specific node
     * @param nodeId Node identifier
     * @return Properties for the node (default if not set)
     */
    static NodeProperties GetNodeProperties(uint32_t nodeId);

private:
    static std::unique_ptr<DelayModel> s_model;
    static bool s_egressEnabled;
    static bool s_ingressEnabled;
};

} // namespace ns3

#endif /* DELAY_HOOKS_H */
