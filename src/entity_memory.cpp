#include "hotbucket/entity_memory.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
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

std::uint32_t ReadUint32(const std::vector<std::uint8_t>& input,
                         std::size_t& offset) {
    if (offset + sizeof(std::uint32_t) > input.size()) {
        throw std::invalid_argument("serialized entity ends inside a uint32 field");
    }
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<std::uint32_t>(input[offset + byte]) << (byte * 8U);
    }
    offset += sizeof(value);
    return value;
}

std::uint64_t ReadUint64(const std::vector<std::uint8_t>& input,
                         std::size_t& offset) {
    if (offset + sizeof(std::uint64_t) > input.size()) {
        throw std::invalid_argument("serialized entity ends inside a uint64 field");
    }
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        value |= static_cast<std::uint64_t>(input[offset + byte]) << (byte * 8U);
    }
    offset += sizeof(value);
    return value;
}

std::string ReadString(const std::vector<std::uint8_t>& input,
                       std::size_t& offset) {
    const auto length = static_cast<std::size_t>(ReadUint32(input, offset));
    if (length > input.size() - offset) {
        throw std::invalid_argument("serialized entity string exceeds its payload");
    }
    const auto begin = input.begin() + static_cast<std::ptrdiff_t>(offset);
    std::string value(begin, begin + static_cast<std::ptrdiff_t>(length));
    offset += length;
    return value;
}

std::vector<std::uint8_t> ReadBinaryFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open entity store: " + path);
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine entity store size: " + path);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty() &&
        !input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("cannot read entity store: " + path);
    }
    return bytes;
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

BattlefieldEntity DeserializeEntity(const std::vector<std::uint8_t>& serialized) {
    std::size_t offset = 0;
    BattlefieldEntity entity;
    entity.entity_id = ReadUint64(serialized, offset);
    entity.callsign = ReadString(serialized, offset);
    entity.entity_type = ReadString(serialized, offset);
    const auto attribute_count = static_cast<std::size_t>(ReadUint32(serialized, offset));
    if (attribute_count > (serialized.size() - offset) / 12) {
        throw std::invalid_argument("serialized entity declares too many attributes");
    }
    entity.attributes.reserve(attribute_count);
    std::unordered_set<std::string> attribute_names;
    for (std::size_t index = 0; index < attribute_count; ++index) {
        EntityAttribute attribute{
            ReadString(serialized, offset),
            ReadString(serialized, offset),
            ReadString(serialized, offset),
        };
        if (attribute.name.empty() || !attribute_names.insert(attribute.name).second) {
            throw std::invalid_argument(
                "serialized entity contains an empty or duplicate attribute key");
        }
        entity.attributes.push_back(std::move(attribute));
    }
    if (offset != serialized.size()) {
        throw std::invalid_argument("serialized entity contains trailing bytes");
    }
    if (entity.entity_id == 0 || entity.callsign.empty() || entity.entity_type.empty()) {
        throw std::invalid_argument("serialized entity is missing required fields");
    }
    return entity;
}

std::vector<BattlefieldEntity> LoadEntityStore(const std::string& path,
                                               std::size_t maximum_entities) {
    constexpr std::size_t header_bytes = 12;
    constexpr std::uint32_t format_version = 1;
    const auto encoded = ReadBinaryFile(path);
    if (encoded.size() < header_bytes || encoded[0] != 'H' || encoded[1] != 'B' ||
        encoded[2] != 'K' || encoded[3] != 'V') {
        throw std::invalid_argument("entity store has an invalid HBKV header");
    }
    std::size_t offset = 4;
    if (ReadUint32(encoded, offset) != format_version) {
        throw std::invalid_argument("entity store version is not supported");
    }
    const auto record_count = static_cast<std::size_t>(ReadUint32(encoded, offset));
    if (record_count == 0) {
        throw std::invalid_argument("entity store contains no records");
    }

    std::vector<BattlefieldEntity> entities;
    entities.reserve(maximum_entities == 0
                         ? record_count
                         : std::min(record_count, maximum_entities));
    std::unordered_set<std::uint64_t> entity_ids;
    for (std::size_t index = 0; index < record_count; ++index) {
        const auto stored_id = ReadUint64(encoded, offset);
        const auto payload_bytes = static_cast<std::size_t>(ReadUint32(encoded, offset));
        if (payload_bytes > encoded.size() - offset) {
            throw std::invalid_argument("entity store record exceeds the file length");
        }
        const auto begin = encoded.begin() + static_cast<std::ptrdiff_t>(offset);
        std::vector<std::uint8_t> payload(
            begin, begin + static_cast<std::ptrdiff_t>(payload_bytes));
        offset += payload_bytes;
        auto entity = DeserializeEntity(payload);
        if (entity.entity_id != stored_id || !entity_ids.insert(stored_id).second) {
            throw std::invalid_argument("entity store contains a mismatched or duplicate key");
        }
        if (maximum_entities == 0 || entities.size() < maximum_entities) {
            entities.push_back(std::move(entity));
        }
    }
    if (offset != encoded.size()) {
        throw std::invalid_argument("entity store contains trailing bytes");
    }
    if (entities.size() < 2) {
        throw std::invalid_argument("at least two stored entities are required");
    }
    std::sort(entities.begin(), entities.end(), [](const auto& left, const auto& right) {
        return left.entity_id < right.entity_id;
    });
    return entities;
}

void SaveEntityStore(const std::string& path,
                     const std::vector<BattlefieldEntity>& entities) {
    constexpr std::uint32_t format_version = 1;
    if (entities.empty() || entities.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("entity store requires a valid non-empty record set");
    }
    std::vector<std::uint8_t> encoded{'H', 'B', 'K', 'V'};
    AppendUint32(encoded, format_version);
    AppendUint32(encoded, static_cast<std::uint32_t>(entities.size()));
    std::unordered_set<std::uint64_t> entity_ids;
    for (const auto& entity : entities) {
        if (entity.entity_id == 0 || !entity_ids.insert(entity.entity_id).second) {
            throw std::invalid_argument("entity store keys must be nonzero and unique");
        }
        const auto payload = SerializeEntity(entity);
        if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("serialized entity exceeds the store format limit");
        }
        AppendUint64(encoded, entity.entity_id);
        AppendUint32(encoded, static_cast<std::uint32_t>(payload.size()));
        encoded.insert(encoded.end(), payload.begin(), payload.end());
    }

    const std::filesystem::path output_path(path);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output ||
        !output.write(reinterpret_cast<const char*>(encoded.data()),
                      static_cast<std::streamsize>(encoded.size()))) {
        throw std::runtime_error("cannot write entity store: " + path);
    }
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
    return node.used_bytes;
}

MemoryPlacementSnapshot EntityMemoryCluster::Allocate(
    const std::vector<BattlefieldEntity>& entities,
    std::size_t detail_limit,
    std::size_t block_entity_limit) {
    for (auto& node : nodes_) {
        std::fill(node.memory.begin(), node.memory.end(), 0);
        node.allocations.clear();
        node.used_bytes = 0;
    }

    std::vector<std::size_t> next_offsets(nodes_.size(), 0);
    for (const auto& entity : entities) {
        const auto payload = SerializeEntity(entity);
        const auto allocated_bytes = AlignUp(payload.size(), alignment_bytes_);
        const auto bucket_id = static_cast<std::size_t>(entity.entity_id % bucket_count_);
        const auto preferred_node = bucket_id % nodes_.size();
        auto node_id = preferred_node;
        auto offset = AlignUp(next_offsets[node_id], alignment_bytes_);
        if (offset + allocated_bytes > nodes_[node_id].memory.size()) {
            node_id = nodes_.size();
            std::size_t least_used = std::numeric_limits<std::size_t>::max();
            for (std::size_t candidate = 0; candidate < nodes_.size(); ++candidate) {
                const auto candidate_offset = AlignUp(next_offsets[candidate], alignment_bytes_);
                if (candidate_offset + allocated_bytes <= nodes_[candidate].memory.size() &&
                    next_offsets[candidate] < least_used) {
                    node_id = candidate;
                    least_used = next_offsets[candidate];
                }
            }
            if (node_id == nodes_.size()) {
                throw std::bad_alloc();
            }
            offset = AlignUp(next_offsets[node_id], alignment_bytes_);
        }
        auto& node = nodes_[node_id];

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
        node.used_bytes += allocated_bytes;
        next_offsets[node_id] = offset + allocated_bytes;
    }

    return Snapshot(detail_limit, block_entity_limit);
}

MemoryPlacementSnapshot EntityMemoryCluster::Snapshot(
    std::size_t detail_limit,
    std::size_t block_entity_limit) const {
    if (block_entity_limit == 0) {
        throw std::invalid_argument("block_entity_limit must be positive");
    }
    MemoryPlacementSnapshot snapshot;
    snapshot.alignment_bytes = alignment_bytes_;
    snapshot.bucket_count = bucket_count_;
    std::size_t allocation_count = 0;
    for (const auto& node : nodes_) {
        allocation_count += node.allocations.size();
    }
    snapshot.detailed = allocation_count <= detail_limit;

    for (std::size_t node_id = 0; node_id < nodes_.size(); ++node_id) {
        const auto& node = nodes_[node_id];
        const auto used_bytes = UsedBytes(node);
        MemoryNodeSnapshot node_snapshot;
        node_snapshot.node_id = node_id;
        node_snapshot.capacity_bytes = node.memory.size();
        node_snapshot.used_bytes = used_bytes;

        MemoryBlockSummary current_block;
        bool has_block = false;
        const auto flush_block = [&]() {
            if (!has_block) {
                return;
            }
            node_snapshot.blocks.push_back(current_block);
            current_block = MemoryBlockSummary{};
            has_block = false;
        };
        for (const auto& allocation : node.allocations) {
            if (allocation.is_replica) {
                ++node_snapshot.replica_count;
                ++snapshot.replica_count;
            } else {
                ++node_snapshot.primary_count;
                ++snapshot.entity_count;
            }

            const auto allocation_end = allocation.offset_bytes + allocation.allocated_bytes;
            const auto block_end = current_block.offset_bytes + current_block.span_bytes;
            const bool same_kind = has_block &&
                                   current_block.is_replica == allocation.is_replica;
            const bool contiguous = has_block && block_end == allocation.offset_bytes;
            const bool same_replica_bucket = !allocation.is_replica ||
                                             (!current_block.mixed_buckets &&
                                              current_block.bucket_id == allocation.bucket_id);
            if (!has_block || !same_kind || !contiguous || !same_replica_bucket ||
                current_block.entity_count >= block_entity_limit) {
                flush_block();
                has_block = true;
                current_block.offset_bytes = allocation.offset_bytes;
                current_block.is_replica = allocation.is_replica;
                current_block.bucket_id = allocation.bucket_id;
                current_block.first_entity_id = allocation.entity.entity_id;
            } else if (current_block.bucket_id != allocation.bucket_id) {
                current_block.mixed_buckets = true;
            }
            current_block.span_bytes = allocation_end - current_block.offset_bytes;
            current_block.used_bytes += allocation.allocated_bytes;
            ++current_block.entity_count;
            current_block.last_entity_id = allocation.entity.entity_id;
        }
        flush_block();

        if (snapshot.detailed) {
            node_snapshot.entities = node.allocations;
        }
        snapshot.total_capacity_bytes += node.memory.size();
        snapshot.total_used_bytes += used_bytes;
        snapshot.nodes.push_back(std::move(node_snapshot));
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

    std::size_t required_bytes = 0;
    for (const auto& source_node : nodes_) {
        for (const auto& source : source_node.allocations) {
            if (source.bucket_id != bucket_id || source.is_replica) {
                continue;
            }
            required_bytes += source.allocated_bytes;
        }
    }
    return required_bytes > 0 &&
           FindFreeOffset(target, required_bytes) != std::numeric_limits<std::size_t>::max();
}

bool EntityMemoryCluster::ReplicateBucket(std::size_t bucket_id,
                                          std::size_t target_node,
                                          std::size_t expires_after_window) {
    if (!CanReplicateBucket(bucket_id, target_node)) {
        return false;
    }

    auto& target = nodes_[target_node];
    std::size_t required_bytes = 0;
    for (const auto& source_node : nodes_) {
        for (const auto& source : source_node.allocations) {
            if (source.bucket_id == bucket_id && !source.is_replica) {
                required_bytes += source.allocated_bytes;
            }
        }
    }
    auto next_offset = FindFreeOffset(target, required_bytes);
    if (required_bytes == 0 || next_offset == std::numeric_limits<std::size_t>::max()) {
        return false;
    }
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
            replica.offset_bytes = next_offset;
            replica.is_replica = true;
            replica.expires_after_window = expires_after_window;
            next_offset += replica.allocated_bytes;
            copies.emplace_back(replica, std::move(payload));
        }
    }

    for (const auto& copy : copies) {
        const auto& allocation = copy.first;
        const auto& payload = copy.second;
        std::copy(payload.begin(), payload.end(),
                  target.memory.begin() + allocation.offset_bytes);
        std::fill(target.memory.begin() + allocation.offset_bytes + allocation.serialized_bytes,
                  target.memory.begin() + allocation.offset_bytes + allocation.allocated_bytes,
                  0);
        target.allocations.push_back(allocation);
        target.used_bytes += allocation.allocated_bytes;
    }
    std::sort(target.allocations.begin(), target.allocations.end(),
              [](const auto& left, const auto& right) {
                  return left.offset_bytes < right.offset_bytes;
              });
    return !copies.empty();
}

std::size_t EntityMemoryCluster::ExpireReplicas(std::size_t window_id) {
    std::size_t expired = 0;
    for (auto& node : nodes_) {
        for (const auto& allocation : node.allocations) {
            if (allocation.is_replica && allocation.expires_after_window < window_id) {
                std::fill(node.memory.begin() + allocation.offset_bytes,
                          node.memory.begin() + allocation.offset_bytes +
                              allocation.allocated_bytes,
                          0);
                node.used_bytes -= allocation.allocated_bytes;
                ++expired;
            }
        }
        node.allocations.erase(
            std::remove_if(node.allocations.begin(), node.allocations.end(),
                           [window_id](const auto& allocation) {
                               return allocation.is_replica &&
                                      allocation.expires_after_window < window_id;
                           }),
            node.allocations.end());
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
