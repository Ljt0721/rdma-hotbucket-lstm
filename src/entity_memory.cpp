#include "hotbucket/entity_memory.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace hotbucket {

namespace {

void AppendUint32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        output.push_back(static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
    }
}

void AppendUint64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        output.push_back(static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
    }
}

void AppendString(std::vector<std::uint8_t>& output, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("entity string is too large to serialize");
    }
    AppendUint32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    return ((value + alignment - 1) / alignment) * alignment;
}

EntityAttribute Attribute(std::string name, std::string type, std::string value) {
    return EntityAttribute{std::move(name), std::move(type), std::move(value)};
}

std::string JoinRecords(const std::string& prefix, std::size_t count) {
    std::string records;
    for (std::size_t index = 0; index < count; ++index) {
        if (!records.empty()) {
            records += ';';
        }
        records += prefix + '-' + std::to_string(index + 1) + ":active";
    }
    return records;
}

}  // namespace

std::vector<std::uint8_t> SerializeEntity(const BattlefieldEntity& entity) {
    std::vector<std::uint8_t> output;
    AppendUint64(output, entity.entity_id);
    AppendString(output, entity.callsign);
    AppendString(output, entity.entity_type);
    AppendUint32(output, static_cast<std::uint32_t>(entity.attributes.size()));
    for (const auto& attribute : entity.attributes) {
        AppendString(output, attribute.name);
        AppendString(output, attribute.value_type);
        AppendString(output, attribute.value);
    }
    return output;
}

std::vector<BattlefieldEntity> MakeBattlefieldScenario(std::size_t entity_count) {
    if (entity_count == 0) {
        throw std::invalid_argument("entity_count must be positive");
    }

    const std::vector<BattlefieldEntity> templates{
        {0, "Blue-HQ", "CommandPost", {
            Attribute("position", "Vector3", "12543.25,8421.70,36.00"),
            Attribute("side", "Enum", "BLUE"),
            Attribute("status", "Enum", "OPERATIONAL"),
            Attribute("command_net", "String", "BLUE-COMMAND-PRIMARY"),
            Attribute("active_orders", "RecordList", JoinRecords("order", 11)),
        }},
        {0, "Viper-1", "MainBattleTank", {
            Attribute("position", "Vector3", "12620.50,8358.10,31.20"),
            Attribute("velocity", "Vector3", "14.20,2.70,0.00"),
            Attribute("health", "Float", "0.94"),
            Attribute("ammunition", "Record", "APFSDS=27;HEAT=16;SMOKE=8"),
            Attribute("fire_control", "RecordList", JoinRecords("track", 7)),
        }},
        {0, "Viper-2", "MainBattleTank", {
            Attribute("position", "Vector3", "12645.80,8334.60,30.90"),
            Attribute("velocity", "Vector3", "12.80,3.10,0.00"),
            Attribute("health", "Float", "0.81"),
            Attribute("ammunition", "Record", "APFSDS=19;HEAT=21;SMOKE=5"),
            Attribute("diagnostics", "RecordList", JoinRecords("subsystem", 13)),
        }},
        {0, "Falcon-3", "ReconUAV", {
            Attribute("position", "Vector3", "13102.40,9014.20,1840.00"),
            Attribute("velocity", "Vector3", "48.00,-6.50,1.20"),
            Attribute("fuel", "Float", "0.67"),
            Attribute("sensor_mode", "Enum", "EO_IR_WIDE"),
            Attribute("route_waypoints", "VectorList", JoinRecords("waypoint", 16)),
            Attribute("image_catalog", "RecordList", JoinRecords("frame", 18)),
        }},
        {0, "Watchtower", "GroundRadar", {
            Attribute("position", "Vector3", "11920.00,8770.00,58.00"),
            Attribute("scan_azimuth", "Float", "287.50"),
            Attribute("range_km", "Float", "180.0"),
            Attribute("operating_band", "Enum", "S_BAND"),
            Attribute("track_table", "RecordList", JoinRecords("air-track", 24)),
        }},
        {0, "Arrow-6", "MissileBattery", {
            Attribute("position", "Vector3", "12114.30,8120.40,42.00"),
            Attribute("readiness", "Enum", "READY"),
            Attribute("launcher_count", "Integer", "4"),
            Attribute("missile_inventory", "Record", "LONG_RANGE=8;MEDIUM_RANGE=12"),
            Attribute("engagement_queue", "RecordList", JoinRecords("candidate", 9)),
        }},
        {0, "Raven-Squad", "InfantrySquad", {
            Attribute("position", "Vector3", "12755.90,8261.30,29.40"),
            Attribute("member_count", "Integer", "9"),
            Attribute("formation", "Enum", "WEDGE"),
            Attribute("combat_load", "Float", "0.76"),
            Attribute("member_state", "RecordList", JoinRecords("soldier", 9)),
        }},
        {0, "Mule-4", "LogisticsTruck", {
            Attribute("position", "Vector3", "12308.10,8184.40,27.80"),
            Attribute("velocity", "Vector3", "8.40,1.10,0.00"),
            Attribute("fuel", "Float", "0.72"),
            Attribute("destination", "EntityRef", "1001"),
            Attribute("cargo_manifest", "RecordList", JoinRecords("supply-crate", 15)),
        }},
        {0, "Mercy-1", "FieldHospital", {
            Attribute("position", "Vector3", "11783.80,8050.20,33.00"),
            Attribute("bed_capacity", "Integer", "42"),
            Attribute("occupied_beds", "Integer", "17"),
            Attribute("triage_state", "Enum", "ELEVATED"),
            Attribute("casualty_board", "RecordList", JoinRecords("patient", 19)),
        }},
        {0, "Meteo-7", "WeatherStation", {
            Attribute("position", "Vector3", "12005.00,8892.50,61.00"),
            Attribute("temperature_c", "Float", "31.4"),
            Attribute("wind", "Vector3", "6.20,-2.10,0.00"),
            Attribute("visibility_km", "Float", "12.7"),
            Attribute("forecast_grid", "RecordList", JoinRecords("cell", 12)),
        }},
    };

    std::vector<BattlefieldEntity> entities;
    entities.reserve(entity_count);
    for (std::size_t index = 0; index < entity_count; ++index) {
        auto entity = templates[index % templates.size()];
        entity.entity_id = 1001 + index;
        if (index >= templates.size()) {
            entity.callsign += '-' + std::to_string(index / templates.size() + 1);
        }
        entities.push_back(std::move(entity));
    }
    return entities;
}

EntityMemoryCluster::EntityMemoryCluster(std::size_t node_count,
                                         std::size_t bucket_count,
                                         std::size_t node_capacity_bytes,
                                         std::size_t alignment_bytes)
    : bucket_count_(bucket_count), alignment_bytes_(alignment_bytes), nodes_(node_count) {
    if (node_count == 0 || bucket_count == 0 || node_capacity_bytes == 0) {
        throw std::invalid_argument("memory cluster dimensions must be positive");
    }
    if (alignment_bytes == 0 || (alignment_bytes & (alignment_bytes - 1)) != 0) {
        throw std::invalid_argument("alignment_bytes must be a power of two");
    }
    for (auto& node : nodes_) {
        node.memory.resize(node_capacity_bytes, 0);
    }
}

std::size_t EntityMemoryCluster::FindTargetNode(std::size_t preferred_node,
                                                std::size_t allocated_bytes) const {
    const auto fits = [&](std::size_t node_id) {
        return FindFreeOffset(nodes_[node_id], allocated_bytes) !=
               std::numeric_limits<std::size_t>::max();
    };
    if (fits(preferred_node)) {
        return preferred_node;
    }

    std::size_t selected = nodes_.size();
    std::size_t selected_used = std::numeric_limits<std::size_t>::max();
    for (std::size_t node_id = 0; node_id < nodes_.size(); ++node_id) {
        const auto used = UsedBytes(nodes_[node_id]);
        if (fits(node_id) && used < selected_used) {
            selected = node_id;
            selected_used = used;
        }
    }
    if (selected == nodes_.size()) {
        throw std::bad_alloc();
    }
    return selected;
}

std::size_t EntityMemoryCluster::FindFreeOffset(const MemoryNode& node,
                                                std::size_t allocated_bytes) const {
    std::vector<const EntityAllocation*> ordered;
    ordered.reserve(node.allocations.size());
    for (const auto& allocation : node.allocations) {
        ordered.push_back(&allocation);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->offset_bytes < right->offset_bytes;
    });

    std::size_t cursor = 0;
    for (const auto* allocation : ordered) {
        const auto candidate = AlignUp(cursor, alignment_bytes_);
        if (candidate + allocated_bytes <= allocation->offset_bytes) {
            return candidate;
        }
        cursor = std::max(cursor, allocation->offset_bytes + allocation->allocated_bytes);
    }

    const auto candidate = AlignUp(cursor, alignment_bytes_);
    if (candidate + allocated_bytes <= node.memory.size()) {
        return candidate;
    }
    return std::numeric_limits<std::size_t>::max();
}

std::size_t EntityMemoryCluster::UsedBytes(const MemoryNode& node) const {
    std::size_t used = 0;
    for (const auto& allocation : node.allocations) {
        used += allocation.allocated_bytes;
    }
    return used;
}

MemoryPlacementSnapshot EntityMemoryCluster::Allocate(
    const std::vector<BattlefieldEntity>& entities) {
    for (auto& node : nodes_) {
        std::fill(node.memory.begin(), node.memory.end(), 0);
        node.allocations.clear();
    }

    for (const auto& entity : entities) {
        const auto payload = SerializeEntity(entity);
        const auto allocated_bytes = AlignUp(payload.size(), alignment_bytes_);
        const auto bucket_id = static_cast<std::size_t>(entity.entity_id % bucket_count_);
        const auto preferred_node = bucket_id % nodes_.size();
        const auto node_id = FindTargetNode(preferred_node, allocated_bytes);
        auto& node = nodes_[node_id];
        const auto offset = FindFreeOffset(node, allocated_bytes);

        std::copy(payload.begin(), payload.end(), node.memory.begin() + offset);
        node.allocations.push_back(EntityAllocation{
            entity,
            bucket_id,
            preferred_node,
            node_id,
            offset,
            payload.size(),
            allocated_bytes,
            false,
            0,
        });
    }

    return Snapshot();
}

MemoryPlacementSnapshot EntityMemoryCluster::Snapshot() const {
    MemoryPlacementSnapshot snapshot;
    snapshot.alignment_bytes = alignment_bytes_;
    snapshot.bucket_count = bucket_count_;
    for (std::size_t node_id = 0; node_id < nodes_.size(); ++node_id) {
        const auto& node = nodes_[node_id];
        const auto used_bytes = UsedBytes(node);
        snapshot.total_capacity_bytes += node.memory.size();
        snapshot.total_used_bytes += used_bytes;
        snapshot.nodes.push_back(MemoryNodeSnapshot{
            node_id,
            node.memory.size(),
            used_bytes,
            node.allocations,
        });
    }
    return snapshot;
}

bool EntityMemoryCluster::CanReplicateBucket(std::size_t bucket_id,
                                             std::size_t target_node) const {
    if (bucket_id >= bucket_count_ || target_node >= nodes_.size() ||
        target_node == bucket_id % nodes_.size()) {
        return false;
    }

    const auto& target = nodes_[target_node];
    const auto already_exists = std::any_of(
        target.allocations.begin(), target.allocations.end(),
        [bucket_id](const EntityAllocation& allocation) {
            return allocation.bucket_id == bucket_id && allocation.is_replica;
        });
    if (already_exists) {
        return false;
    }

    auto planned = target;
    bool found_primary = false;
    for (const auto& source_node : nodes_) {
        for (const auto& source : source_node.allocations) {
            if (source.bucket_id != bucket_id || source.is_replica) {
                continue;
            }
            found_primary = true;
            const auto offset = FindFreeOffset(planned, source.allocated_bytes);
            if (offset == std::numeric_limits<std::size_t>::max()) {
                return false;
            }
            auto allocation = source;
            allocation.node_id = target_node;
            allocation.offset_bytes = offset;
            allocation.is_replica = true;
            planned.allocations.push_back(std::move(allocation));
        }
    }
    return found_primary;
}

bool EntityMemoryCluster::ReplicateBucket(std::size_t bucket_id,
                                          std::size_t target_node,
                                          std::size_t expires_after_window) {
    if (!CanReplicateBucket(bucket_id, target_node)) {
        return false;
    }

    auto& target = nodes_[target_node];
    auto planned = target;
    std::vector<std::pair<EntityAllocation, std::vector<std::uint8_t>>> copies;
    for (const auto& source_node : nodes_) {
        for (const auto& source : source_node.allocations) {
            if (source.bucket_id != bucket_id || source.is_replica) {
                continue;
            }
            const auto begin = source_node.memory.begin() + source.offset_bytes;
            std::vector<std::uint8_t> payload(begin, begin + source.serialized_bytes);
            auto replica = source;
            replica.node_id = target_node;
            replica.offset_bytes = FindFreeOffset(planned, replica.allocated_bytes);
            replica.is_replica = true;
            replica.expires_after_window = expires_after_window;
            planned.allocations.push_back(replica);
            copies.emplace_back(replica, std::move(payload));
        }
    }

    target.allocations = std::move(planned.allocations);
    for (const auto& copy : copies) {
        const auto& allocation = copy.first;
        const auto& payload = copy.second;
        std::copy(payload.begin(), payload.end(),
                  target.memory.begin() + allocation.offset_bytes);
        std::fill(target.memory.begin() + allocation.offset_bytes + allocation.serialized_bytes,
                  target.memory.begin() + allocation.offset_bytes + allocation.allocated_bytes,
                  0);
    }
    return !copies.empty();
}

std::size_t EntityMemoryCluster::ExpireReplicas(std::size_t window_id) {
    std::size_t expired = 0;
    for (auto& node : nodes_) {
        auto allocation = node.allocations.begin();
        while (allocation != node.allocations.end()) {
            if (allocation->is_replica && allocation->expires_after_window < window_id) {
                std::fill(node.memory.begin() + allocation->offset_bytes,
                          node.memory.begin() + allocation->offset_bytes +
                              allocation->allocated_bytes,
                          0);
                allocation = node.allocations.erase(allocation);
                ++expired;
            } else {
                ++allocation;
            }
        }
    }
    return expired;
}

std::size_t EntityMemoryCluster::PrimaryBytesInBucket(std::size_t bucket_id) const {
    std::size_t bytes = 0;
    for (const auto& node : nodes_) {
        for (const auto& allocation : node.allocations) {
            if (!allocation.is_replica && allocation.bucket_id == bucket_id) {
                bytes += allocation.allocated_bytes;
            }
        }
    }
    return bytes;
}

ReplicaBundle EntityMemoryCluster::BuildReplicaBundle(
    std::size_t bucket_id,
    std::size_t target_node,
    std::size_t expires_after_window) const {
    if (!CanReplicateBucket(bucket_id, target_node)) {
        throw std::invalid_argument("bucket cannot be replicated to the requested target node");
    }

    ReplicaBundle bundle;
    bundle.bucket_id = bucket_id;
    bundle.target_node = target_node;
    bundle.expires_after_window = expires_after_window;
    for (std::size_t source_node = 0; source_node < nodes_.size(); ++source_node) {
        const auto& node = nodes_[source_node];
        for (const auto& allocation : node.allocations) {
            if (allocation.bucket_id != bucket_id || allocation.is_replica) {
                continue;
            }
            if (allocation.offset_bytes + allocation.allocated_bytes > node.memory.size()) {
                throw std::logic_error("primary allocation extends beyond its memory node");
            }
            const auto begin = node.memory.begin() + allocation.offset_bytes;
            bundle.records.push_back(ReplicaRecord{
                allocation.entity.entity_id,
                source_node,
                allocation.serialized_bytes,
                allocation.allocated_bytes,
                std::vector<std::uint8_t>(begin, begin + allocation.allocated_bytes),
            });
        }
    }
    if (bundle.records.empty()) {
        throw std::logic_error("replica bundle contains no primary allocations");
    }
    return bundle;
}

std::vector<std::uint8_t> EntityMemoryCluster::ReadSerializedEntity(
    std::uint64_t entity_id) const {
    for (const auto& node : nodes_) {
        const auto allocation = std::find_if(
            node.allocations.begin(), node.allocations.end(),
            [entity_id](const EntityAllocation& candidate) {
                return candidate.entity.entity_id == entity_id && !candidate.is_replica;
            });
        if (allocation != node.allocations.end()) {
            const auto begin = node.memory.begin() + allocation->offset_bytes;
            return std::vector<std::uint8_t>(begin, begin + allocation->serialized_bytes);
        }
    }
    throw std::out_of_range("entity is not allocated in the memory cluster");
}

std::vector<std::uint8_t> EntityMemoryCluster::ReadSerializedEntity(
    std::uint64_t entity_id,
    std::size_t node_id,
    bool replica) const {
    if (node_id >= nodes_.size()) {
        throw std::out_of_range("memory node is outside the configured range");
    }
    const auto& node = nodes_[node_id];
    const auto allocation = std::find_if(
        node.allocations.begin(), node.allocations.end(),
        [entity_id, replica](const EntityAllocation& candidate) {
            return candidate.entity.entity_id == entity_id &&
                   candidate.is_replica == replica;
        });
    if (allocation == node.allocations.end()) {
        throw std::out_of_range("entity copy is not allocated on the requested node");
    }
    const auto begin = node.memory.begin() + allocation->offset_bytes;
    return std::vector<std::uint8_t>(begin, begin + allocation->serialized_bytes);
}

}  // namespace hotbucket
