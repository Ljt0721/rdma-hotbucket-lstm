#pragma once

#include "hotbucket/types.hpp"

#include <cstddef>
#include <vector>

namespace hotbucket {

class ClusterSimulator {
public:
    explicit ClusterSimulator(const SimulationConfig& config);

    void BeginWindow(std::size_t window_id);
    bool ApplyReplication(const ReplicationDecision& decision,
                          std::size_t window_id,
                          double* actual_copy_cost_ms);
    WindowResult ProcessWindow(std::size_t window_id,
                               std::size_t generated_hot_bucket,
                               const std::vector<Request>& requests,
                               double copy_cost_ms) const;

    bool HasReplica(std::size_t bucket_id) const;
    bool HasReplicaOn(std::size_t bucket_id, std::size_t node_id) const;
    std::size_t HomeNode(std::size_t bucket_id) const;
    std::size_t ReplicaCount(std::size_t bucket_id) const;
    std::size_t LeastLoadedAlternative(std::size_t bucket_id,
                                       const std::vector<std::size_t>& previous_node_operations) const;
    double EstimateCopyCostMs() const;
    const SimulationConfig& Config() const;

private:
    struct Replica {
        std::size_t node_id{};
        std::size_t expires_after_window{};
    };

    std::vector<std::size_t> ReadLocations(std::size_t bucket_id) const;

    const SimulationConfig& config_;
    std::vector<std::vector<Replica>> replicas_;
};

}  // namespace hotbucket
