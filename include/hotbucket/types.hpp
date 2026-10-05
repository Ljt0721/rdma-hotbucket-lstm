#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hotbucket {

enum class Operation {
    Get,
    Put,
};

struct Request {
    std::uint64_t entity_id{};
    std::size_t bucket_id{};
    Operation operation{Operation::Get};
};

struct SimulationConfig {
    std::size_t node_count{5};
    std::size_t bucket_count{64};
    std::size_t entity_count{10};
    std::size_t window_count{40};
    std::size_t requests_per_window{4000};
    std::size_t hotspot_duration_windows{5};
    std::size_t node_capacity_per_window{1800};
    std::size_t replica_ttl_windows{3};
    std::string workload_pattern{"step"};
    double read_ratio{0.90};
    double hotspot_share{0.65};
    double base_latency_us{5.0};
    double bucket_size_mib{4.0};
    double copy_bandwidth_mib_per_ms{1.5};
    double metadata_cost_ms{0.08};
    std::uint64_t seed{42};
};

struct ReplicationDecision {
    std::size_t bucket_id{};
    std::size_t target_node{};
    double predicted_gets{};
    double expected_benefit_ms{};
    double expected_cost_ms{};
    std::string reason;
    double hotspot_probability{};
};

struct ReplicaLocation {
    std::size_t bucket_id{};
    std::size_t node_id{};
};

struct BucketWindowMetrics {
    std::size_t bucket_id{};
    std::size_t home_node{};
    std::size_t get_count{};
    std::size_t put_count{};
    std::size_t replica_count{};
    double mean_latency_us{};
    double p99_latency_us{};
    double home_node_load_ratio{};
    bool generated_hot_bucket{false};
};

struct WindowResult {
    std::size_t window_id{};
    std::size_t generated_hot_bucket{};
    std::vector<BucketWindowMetrics> buckets;
    std::vector<std::size_t> node_operations;
    double cluster_max_load_ratio{};
    double window_completion_ms{};
    double copy_cost_ms{};
    double control_overhead_ms{};
    bool rdma_copy{false};
    std::size_t copied_bytes{};
    std::uint64_t rdma_sequence{};
    double rdma_write_ms{};
    double rdma_end_to_end_ms{};
    std::optional<ReplicationDecision> decision;
    bool copy_applied{false};
};

}  // namespace hotbucket
