#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hotbucket {

struct RdmaReplicationResult {
    std::size_t bucket_id{};
    std::size_t target_node{};
    std::size_t transferred_bytes{};
    double write_completion_ms{};
    double end_to_end_ms{};
    std::string provider;
};

RdmaReplicationResult ReplicateBundleRdma(
    const std::string& server_ip,
    std::uint16_t port,
    const std::vector<std::uint8_t>& encoded_bundle);

void RunRdmaReplicaServer(std::uint16_t port,
                          std::size_t node_id,
                          std::size_t maximum_replications,
                          std::size_t maximum_bundle_bytes);

}  // namespace hotbucket
