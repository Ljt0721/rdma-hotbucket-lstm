#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hotbucket {

struct ReplicaRecord {
    std::uint64_t entity_id{};
    std::size_t source_node{};
    std::size_t serialized_bytes{};
    std::size_t allocated_bytes{};
    std::vector<std::uint8_t> bytes;
};

struct ReplicaBundle {
    std::size_t bucket_id{};
    std::size_t target_node{};
    std::size_t expires_after_window{};
    std::vector<ReplicaRecord> records;
};

std::vector<std::uint8_t> EncodeReplicaBundle(const ReplicaBundle& bundle);
ReplicaBundle DecodeReplicaBundle(const std::vector<std::uint8_t>& encoded);
std::uint32_t ReplicaBundleChecksum(const std::vector<std::uint8_t>& encoded);

}  // namespace hotbucket
