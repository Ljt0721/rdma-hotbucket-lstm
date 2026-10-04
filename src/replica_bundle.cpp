#include "hotbucket/replica_bundle.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace hotbucket {

namespace {

constexpr std::uint32_t kBundleMagic = 0x48425250U;
constexpr std::uint16_t kBundleVersion = 1;
constexpr std::uint16_t kHeaderBytes = 40;
constexpr std::size_t kChecksumOffset = 12;
constexpr std::size_t kRecordHeaderBytes = 24;
constexpr std::uint32_t kFnvOffsetBasis = 2166136261U;
constexpr std::uint32_t kFnvPrime = 16777619U;

void RequireUint32(std::size_t value, const char* name) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error(std::string(name) + " exceeds the wire-format limit");
    }
}

void AppendUint16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void AppendUint32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void AppendUint64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void SetUint32(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    if (offset + sizeof(value) > output.size()) {
        throw std::out_of_range("wire-field offset is outside the encoded bundle");
    }
    output[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    output[offset + 1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    output[offset + 2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    output[offset + 3] = static_cast<std::uint8_t>(value & 0xffU);
}

std::uint16_t ReadUint16(const std::vector<std::uint8_t>& input, std::size_t& offset) {
    if (offset + 2 > input.size()) {
        throw std::invalid_argument("replica bundle ends inside a uint16 field");
    }
    const auto value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(input[offset]) << 8U) |
        static_cast<std::uint16_t>(input[offset + 1]));
    offset += 2;
    return value;
}

std::uint32_t ReadUint32(const std::vector<std::uint8_t>& input, std::size_t& offset) {
    if (offset + 4 > input.size()) {
        throw std::invalid_argument("replica bundle ends inside a uint32 field");
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value = (value << 8U) | static_cast<std::uint32_t>(input[offset + index]);
    }
    offset += 4;
    return value;
}

std::uint64_t ReadUint64(const std::vector<std::uint8_t>& input, std::size_t& offset) {
    if (offset + 8 > input.size()) {
        throw std::invalid_argument("replica bundle ends inside a uint64 field");
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | static_cast<std::uint64_t>(input[offset + index]);
    }
    offset += 8;
    return value;
}

}  // namespace

std::uint32_t ReplicaBundleChecksum(const std::vector<std::uint8_t>& encoded) {
    if (encoded.size() < kHeaderBytes) {
        throw std::invalid_argument("replica bundle is shorter than its header");
    }
    std::uint32_t checksum = kFnvOffsetBasis;
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const std::uint8_t byte =
            index >= kChecksumOffset && index < kChecksumOffset + 4 ? 0 : encoded[index];
        checksum ^= byte;
        checksum *= kFnvPrime;
    }
    return checksum;
}

std::vector<std::uint8_t> EncodeReplicaBundle(const ReplicaBundle& bundle) {
    RequireUint32(bundle.bucket_id, "bucket_id");
    RequireUint32(bundle.target_node, "target_node");
    RequireUint32(bundle.records.size(), "record count");

    std::vector<std::uint8_t> encoded;
    encoded.reserve(kHeaderBytes);
    AppendUint32(encoded, kBundleMagic);
    AppendUint16(encoded, kBundleVersion);
    AppendUint16(encoded, kHeaderBytes);
    AppendUint32(encoded, 0);
    AppendUint32(encoded, 0);
    AppendUint32(encoded, static_cast<std::uint32_t>(bundle.bucket_id));
    AppendUint32(encoded, static_cast<std::uint32_t>(bundle.target_node));
    AppendUint64(encoded, static_cast<std::uint64_t>(bundle.expires_after_window));
    AppendUint32(encoded, static_cast<std::uint32_t>(bundle.records.size()));
    AppendUint32(encoded, 0);

    std::unordered_set<std::uint64_t> entity_ids;
    for (const auto& record : bundle.records) {
        RequireUint32(record.source_node, "source_node");
        RequireUint32(record.serialized_bytes, "serialized_bytes");
        RequireUint32(record.allocated_bytes, "allocated_bytes");
        if (!entity_ids.insert(record.entity_id).second) {
            throw std::invalid_argument("replica bundle contains a duplicate entity_id");
        }
        if (record.serialized_bytes > record.allocated_bytes ||
            record.bytes.size() != record.allocated_bytes) {
            throw std::invalid_argument("replica record sizes do not match its payload");
        }
        AppendUint64(encoded, record.entity_id);
        AppendUint32(encoded, static_cast<std::uint32_t>(record.source_node));
        AppendUint32(encoded, static_cast<std::uint32_t>(record.serialized_bytes));
        AppendUint32(encoded, static_cast<std::uint32_t>(record.allocated_bytes));
        AppendUint32(encoded, 0);
        encoded.insert(encoded.end(), record.bytes.begin(), record.bytes.end());
    }

    RequireUint32(encoded.size(), "encoded replica bundle");
    SetUint32(encoded, 8, static_cast<std::uint32_t>(encoded.size()));
    SetUint32(encoded, kChecksumOffset, ReplicaBundleChecksum(encoded));
    return encoded;
}

ReplicaBundle DecodeReplicaBundle(const std::vector<std::uint8_t>& encoded) {
    if (encoded.size() < kHeaderBytes) {
        throw std::invalid_argument("replica bundle is shorter than its header");
    }
    std::size_t offset = 0;
    if (ReadUint32(encoded, offset) != kBundleMagic) {
        throw std::invalid_argument("replica bundle has the wrong magic value");
    }
    if (ReadUint16(encoded, offset) != kBundleVersion) {
        throw std::invalid_argument("replica bundle version is not supported");
    }
    if (ReadUint16(encoded, offset) != kHeaderBytes) {
        throw std::invalid_argument("replica bundle has an unexpected header length");
    }
    if (ReadUint32(encoded, offset) != encoded.size()) {
        throw std::invalid_argument("replica bundle length field does not match received bytes");
    }
    const std::uint32_t stored_checksum = ReadUint32(encoded, offset);
    if (stored_checksum != ReplicaBundleChecksum(encoded)) {
        throw std::invalid_argument("replica bundle checksum does not match its contents");
    }

    ReplicaBundle bundle;
    bundle.bucket_id = ReadUint32(encoded, offset);
    bundle.target_node = ReadUint32(encoded, offset);
    bundle.expires_after_window = static_cast<std::size_t>(ReadUint64(encoded, offset));
    const std::size_t record_count = ReadUint32(encoded, offset);
    static_cast<void>(ReadUint32(encoded, offset));
    if (record_count > (encoded.size() - kHeaderBytes) / kRecordHeaderBytes) {
        throw std::invalid_argument("replica bundle record count exceeds its byte length");
    }

    bundle.records.reserve(record_count);
    std::unordered_set<std::uint64_t> entity_ids;
    for (std::size_t index = 0; index < record_count; ++index) {
        ReplicaRecord record;
        record.entity_id = ReadUint64(encoded, offset);
        record.source_node = ReadUint32(encoded, offset);
        record.serialized_bytes = ReadUint32(encoded, offset);
        record.allocated_bytes = ReadUint32(encoded, offset);
        static_cast<void>(ReadUint32(encoded, offset));
        if (!entity_ids.insert(record.entity_id).second) {
            throw std::invalid_argument("replica bundle contains a duplicate entity_id");
        }
        if (record.serialized_bytes > record.allocated_bytes ||
            record.allocated_bytes > encoded.size() - offset) {
            throw std::invalid_argument("replica record extends beyond the received bundle");
        }
        record.bytes.assign(
            encoded.begin() + static_cast<std::ptrdiff_t>(offset),
            encoded.begin() + static_cast<std::ptrdiff_t>(offset + record.allocated_bytes));
        offset += record.allocated_bytes;
        bundle.records.push_back(std::move(record));
    }
    if (offset != encoded.size()) {
        throw std::invalid_argument("replica bundle contains trailing bytes");
    }
    if (bundle.records.empty()) {
        throw std::invalid_argument("replica bundle contains no entity records");
    }
    return bundle;
}

}  // namespace hotbucket
