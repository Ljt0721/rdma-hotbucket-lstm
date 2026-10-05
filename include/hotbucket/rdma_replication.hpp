#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hotbucket {

inline constexpr std::size_t kDefaultRdmaStagingBytes = 8U * 1024U * 1024U;

struct RdmaReplicationResult {
    std::size_t bucket_id{};
    std::size_t target_node{};
    std::size_t transferred_bytes{};
    std::uint64_t sequence_number{};
    double write_completion_ms{};
    double end_to_end_ms{};
    std::string provider;
};

class RdmaReplicaSession {
public:
    RdmaReplicaSession(const std::string& server_ip,
                       std::uint16_t port,
                       std::size_t target_node,
                       std::size_t staging_bytes = kDefaultRdmaStagingBytes);
    ~RdmaReplicaSession();

    RdmaReplicaSession(const RdmaReplicaSession&) = delete;
    RdmaReplicaSession& operator=(const RdmaReplicaSession&) = delete;
    RdmaReplicaSession(RdmaReplicaSession&&) noexcept;
    RdmaReplicaSession& operator=(RdmaReplicaSession&&) noexcept;

    RdmaReplicationResult Replicate(
        const std::vector<std::uint8_t>& encoded_bundle);
    std::size_t target_node() const noexcept;
    std::size_t staging_bytes() const noexcept;
    double setup_ms() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
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
