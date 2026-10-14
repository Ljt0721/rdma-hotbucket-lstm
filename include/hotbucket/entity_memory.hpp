#pragma once

#include "hotbucket/replica_bundle.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace hotbucket {

struct EntityAttribute {
    std::string name;
    std::string value_type;
    std::string value;
};

struct BattlefieldEntity {
    std::uint64_t entity_id{};
    std::string callsign;
    std::string entity_type;
    std::vector<EntityAttribute> attributes;
};

struct EntityAllocation {
    BattlefieldEntity entity;
    std::size_t bucket_id{};
    std::size_t home_node{};
    std::size_t node_id{};
    std::size_t offset_bytes{};
    std::size_t serialized_bytes{};
    std::size_t allocated_bytes{};
    bool is_replica{false};
    std::size_t expires_after_window{};
};

struct MemoryBlockSummary {
    std::size_t offset_bytes{};
    std::size_t span_bytes{};
    std::size_t used_bytes{};
    std::size_t entity_count{};
    bool is_replica{false};
    bool mixed_buckets{false};
    std::size_t bucket_id{};
    std::uint64_t first_entity_id{};
    std::uint64_t last_entity_id{};
};

struct MemoryNodeSnapshot {
    std::size_t node_id{};
    std::size_t capacity_bytes{};
    std::size_t used_bytes{};
    std::size_t primary_count{};
    std::size_t replica_count{};
    std::vector<MemoryBlockSummary> blocks;
    std::vector<EntityAllocation> entities;
};

struct MemoryPlacementSnapshot {
    bool detailed{true};
    std::size_t alignment_bytes{};
    std::size_t bucket_count{};
    std::size_t entity_count{};
    std::size_t replica_count{};
    std::size_t total_capacity_bytes{};
    std::size_t total_used_bytes{};
    std::vector<MemoryNodeSnapshot> nodes;
};

class EntityMemoryCluster {
public:
    EntityMemoryCluster(std::size_t node_count,
                        std::size_t bucket_count,
                        std::size_t node_capacity_bytes = 2048,
                        std::size_t alignment_bytes = 64);

    MemoryPlacementSnapshot Allocate(
        const std::vector<BattlefieldEntity>& entities,
        std::size_t detail_limit = std::numeric_limits<std::size_t>::max(),
        std::size_t block_entity_limit = 10000);
    MemoryPlacementSnapshot Snapshot(
        std::size_t detail_limit = std::numeric_limits<std::size_t>::max(),
        std::size_t block_entity_limit = 10000) const;
    bool CanReplicateBucket(std::size_t bucket_id, std::size_t target_node) const;
    bool ReplicateBucket(std::size_t bucket_id,
                         std::size_t target_node,
                         std::size_t expires_after_window);
    std::size_t ExpireReplicas(std::size_t window_id);
    std::size_t PrimaryBytesInBucket(std::size_t bucket_id) const;
    ReplicaBundle BuildReplicaBundle(std::size_t bucket_id,
                                     std::size_t target_node,
                                     std::size_t expires_after_window) const;
    std::vector<std::uint8_t> ReadSerializedEntity(std::uint64_t entity_id) const;
    std::vector<std::uint8_t> ReadSerializedEntity(std::uint64_t entity_id,
                                                   std::size_t node_id,
                                                   bool replica) const;

private:
    struct MemoryNode {
        std::vector<std::uint8_t> memory;
        std::vector<EntityAllocation> allocations;
        std::size_t used_bytes{};
    };

    std::size_t FindTargetNode(std::size_t preferred_node,
                               std::size_t allocated_bytes) const;
    std::size_t FindFreeOffset(const MemoryNode& node, std::size_t allocated_bytes) const;
    std::size_t UsedBytes(const MemoryNode& node) const;

    std::size_t bucket_count_{};
    std::size_t alignment_bytes_{};
    std::vector<MemoryNode> nodes_;
};

std::vector<BattlefieldEntity> MakeBattlefieldScenario(std::size_t entity_count);
std::vector<std::uint8_t> SerializeEntity(const BattlefieldEntity& entity);
BattlefieldEntity DeserializeEntity(const std::vector<std::uint8_t>& serialized);
std::vector<BattlefieldEntity> LoadEntityStore(const std::string& path,
                                               std::size_t maximum_entities = 0);
void SaveEntityStore(const std::string& path,
                     const std::vector<BattlefieldEntity>& entities);

}  // namespace hotbucket
