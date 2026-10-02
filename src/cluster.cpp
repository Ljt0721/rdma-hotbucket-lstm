#include "hotbucket/cluster.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace hotbucket {

namespace {

double MeanLatencyUs(double base_latency_us, double load_ratio) {
    const double safe_ratio = std::min(load_ratio, 0.98);
    const double queue_factor = 1.0 + safe_ratio / std::max(0.05, 1.0 - safe_ratio);
    const double overload_factor = load_ratio > 1.0 ? 1.0 + 8.0 * (load_ratio - 1.0) : 1.0;
    return base_latency_us * queue_factor * overload_factor;
}

}  // namespace

ClusterSimulator::ClusterSimulator(const SimulationConfig& config)
    : config_(config), replicas_(config.bucket_count) {
    if (config_.node_count < 2) {
        throw std::invalid_argument("node_count must be at least 2");
    }
    if (config_.node_capacity_per_window == 0) {
        throw std::invalid_argument("node_capacity_per_window must be positive");
    }
    if (config_.copy_bandwidth_mib_per_ms <= 0.0) {
        throw std::invalid_argument("copy_bandwidth_mib_per_ms must be positive");
    }
    if (config_.bucket_size_mib < 0.0 || config_.metadata_cost_ms < 0.0) {
        throw std::invalid_argument("copy size and metadata cost cannot be negative");
    }
}

std::vector<ReplicaLocation> ClusterSimulator::BeginWindow(std::size_t window_id) {
    std::vector<ReplicaLocation> expired;
    for (std::size_t bucket_id = 0; bucket_id < replicas_.size(); ++bucket_id) {
        auto& bucket_replicas = replicas_[bucket_id];
        auto replica = bucket_replicas.begin();
        while (replica != bucket_replicas.end()) {
            if (replica->expires_after_window < window_id) {
                expired.push_back(ReplicaLocation{bucket_id, replica->node_id});
                replica = bucket_replicas.erase(replica);
            } else {
                ++replica;
            }
        }
    }
    return expired;
}

bool ClusterSimulator::ApplyReplication(const ReplicationDecision& decision,
                                        std::size_t window_id,
                                        double* actual_copy_cost_ms) {
    if (decision.bucket_id >= config_.bucket_count || decision.target_node >= config_.node_count) {
        return false;
    }
    if (decision.target_node == HomeNode(decision.bucket_id)) {
        return false;
    }

    auto& bucket_replicas = replicas_[decision.bucket_id];
    auto existing = std::find_if(bucket_replicas.begin(), bucket_replicas.end(),
                                 [&decision](const Replica& replica) {
                                     return replica.node_id == decision.target_node;
                                 });
    if (existing != bucket_replicas.end()) {
        existing->expires_after_window = window_id + config_.replica_ttl_windows;
        return false;
    }

    bucket_replicas.push_back(Replica{
        decision.target_node,
        window_id + config_.replica_ttl_windows,
    });
    if (actual_copy_cost_ms != nullptr) {
        *actual_copy_cost_ms = EstimateCopyCostMs(decision.bucket_id);
    }
    return true;
}

WindowResult ClusterSimulator::ProcessWindow(std::size_t window_id,
                                             std::size_t generated_hot_bucket,
                                             const std::vector<Request>& requests,
                                             double copy_cost_ms,
                                             double control_overhead_ms) const {
    std::vector<std::vector<std::size_t>> get_routes(
        config_.bucket_count, std::vector<std::size_t>(config_.node_count, 0));
    std::vector<std::size_t> put_counts(config_.bucket_count, 0);
    std::vector<std::size_t> get_counts(config_.bucket_count, 0);
    std::vector<std::size_t> node_operations(config_.node_count, 0);

    for (const auto& request : requests) {
        if (request.bucket_id >= config_.bucket_count) {
            throw std::out_of_range("request bucket_id is outside the configured range");
        }

        const auto locations = ReadLocations(request.bucket_id);
        if (request.operation == Operation::Get) {
            const auto node = *std::min_element(
                locations.begin(), locations.end(),
                [&node_operations](std::size_t left, std::size_t right) {
                    return node_operations[left] < node_operations[right];
                });
            ++get_counts[request.bucket_id];
            ++get_routes[request.bucket_id][node];
            ++node_operations[node];
        } else {
            ++put_counts[request.bucket_id];
            // A read replica must receive the new value before later reads use it.
            for (const auto node : locations) {
                ++node_operations[node];
            }
        }
    }

    std::vector<double> node_load_ratio(config_.node_count, 0.0);
    std::vector<double> node_mean_latency(config_.node_count, config_.base_latency_us);
    double max_load_ratio = 0.0;
    double max_node_completion_ms = 0.0;
    for (std::size_t node = 0; node < config_.node_count; ++node) {
        node_load_ratio[node] = static_cast<double>(node_operations[node]) /
                                static_cast<double>(config_.node_capacity_per_window);
        node_mean_latency[node] = MeanLatencyUs(config_.base_latency_us, node_load_ratio[node]);
        max_load_ratio = std::max(max_load_ratio, node_load_ratio[node]);
        const double completion_ms =
            static_cast<double>(node_operations[node]) * node_mean_latency[node] / 1000.0;
        max_node_completion_ms = std::max(max_node_completion_ms, completion_ms);
    }

    std::vector<BucketWindowMetrics> bucket_metrics;
    bucket_metrics.reserve(config_.bucket_count);
    for (std::size_t bucket = 0; bucket < config_.bucket_count; ++bucket) {
        const auto locations = ReadLocations(bucket);
        const auto logical_operations = get_counts[bucket] + put_counts[bucket];
        double latency_sum = 0.0;
        double bucket_p99 = config_.base_latency_us;

        for (std::size_t node = 0; node < config_.node_count; ++node) {
            latency_sum += static_cast<double>(get_routes[bucket][node]) * node_mean_latency[node];
            if (get_routes[bucket][node] > 0) {
                bucket_p99 = std::max(bucket_p99,
                                      node_mean_latency[node] * (1.0 + 2.0 * node_load_ratio[node]));
            }
        }

        if (put_counts[bucket] > 0) {
            double write_latency = 0.0;
            for (const auto node : locations) {
                write_latency = std::max(write_latency, node_mean_latency[node]);
                bucket_p99 = std::max(bucket_p99,
                                      node_mean_latency[node] * (1.0 + 2.0 * node_load_ratio[node]));
            }
            latency_sum += static_cast<double>(put_counts[bucket]) * write_latency;
        }

        const double mean_latency = logical_operations == 0
                                        ? config_.base_latency_us
                                        : latency_sum / static_cast<double>(logical_operations);
        bucket_metrics.push_back(BucketWindowMetrics{
            bucket,
            HomeNode(bucket),
            get_counts[bucket],
            put_counts[bucket],
            ReplicaCount(bucket),
            mean_latency,
            bucket_p99,
            node_load_ratio[HomeNode(bucket)],
            bucket == generated_hot_bucket,
        });
    }

    WindowResult result;
    result.window_id = window_id;
    result.generated_hot_bucket = generated_hot_bucket;
    result.buckets = std::move(bucket_metrics);
    result.node_operations = std::move(node_operations);
    result.cluster_max_load_ratio = max_load_ratio;
    result.window_completion_ms =
        max_node_completion_ms + copy_cost_ms + control_overhead_ms;
    result.copy_cost_ms = copy_cost_ms;
    result.control_overhead_ms = control_overhead_ms;
    return result;
}

bool ClusterSimulator::HasReplica(std::size_t bucket_id) const {
    return !replicas_.at(bucket_id).empty();
}

bool ClusterSimulator::HasReplicaOn(std::size_t bucket_id, std::size_t node_id) const {
    const auto& bucket_replicas = replicas_.at(bucket_id);
    return std::any_of(bucket_replicas.begin(), bucket_replicas.end(),
                       [node_id](const Replica& replica) { return replica.node_id == node_id; });
}

std::size_t ClusterSimulator::HomeNode(std::size_t bucket_id) const {
    return bucket_id % config_.node_count;
}

std::size_t ClusterSimulator::ReplicaCount(std::size_t bucket_id) const {
    return replicas_.at(bucket_id).size();
}

std::size_t ClusterSimulator::LeastLoadedAlternative(
    std::size_t bucket_id,
    const std::vector<std::size_t>& previous_node_operations) const {
    if (previous_node_operations.size() != config_.node_count) {
        throw std::invalid_argument("node load vector has the wrong size");
    }

    const auto home = HomeNode(bucket_id);
    std::size_t selected = home;
    std::size_t selected_load = std::numeric_limits<std::size_t>::max();
    for (std::size_t node = 0; node < config_.node_count; ++node) {
        if (node == home || HasReplicaOn(bucket_id, node)) {
            continue;
        }
        if (previous_node_operations[node] < selected_load) {
            selected = node;
            selected_load = previous_node_operations[node];
        }
    }
    return selected;
}

double ClusterSimulator::EstimateCopyCostMs() const {
    return config_.bucket_size_mib / config_.copy_bandwidth_mib_per_ms +
           config_.metadata_cost_ms;
}

double ClusterSimulator::EstimateCopyCostMs(std::size_t bucket_id) const {
    if (bucket_id >= config_.bucket_count) {
        throw std::out_of_range("bucket_id is outside the configured range");
    }
    constexpr double bytes_per_mib = 1024.0 * 1024.0;
    double measured_bucket_mib = 0.0;
    if (bucket_bytes_.size() == config_.bucket_count) {
        measured_bucket_mib = static_cast<double>(bucket_bytes_[bucket_id]) / bytes_per_mib;
    }
    // The ten visible entities are representative records, not the full logical bucket payload.
    const double bucket_mib = std::max(config_.bucket_size_mib, measured_bucket_mib);
    return bucket_mib / config_.copy_bandwidth_mib_per_ms + config_.metadata_cost_ms;
}

double ClusterSimulator::EstimateWindowCompletionMs(
    const std::vector<double>& node_operations) const {
    if (node_operations.size() != config_.node_count) {
        throw std::invalid_argument("node operation vector has the wrong size");
    }
    double completion_ms = 0.0;
    for (const double operations : node_operations) {
        const double load_ratio = operations /
                                  static_cast<double>(config_.node_capacity_per_window);
        const double mean_latency_us = MeanLatencyUs(config_.base_latency_us, load_ratio);
        completion_ms = std::max(
            completion_ms,
            operations * mean_latency_us / 1000.0);
    }
    return completion_ms;
}

double ClusterSimulator::EstimateReplicationBenefitMs(
    std::size_t bucket_id,
    double predicted_gets,
    std::size_t target_node,
    const std::vector<BucketWindowMetrics>& previous_buckets) const {
    if (bucket_id >= config_.bucket_count || target_node >= config_.node_count) {
        throw std::out_of_range("replication estimate is outside the configured cluster");
    }
    if (previous_buckets.size() != config_.bucket_count) {
        throw std::invalid_argument("bucket metric vector has the wrong size");
    }

    const auto estimate_completion = [&](bool add_candidate_replica) {
        std::vector<double> node_operations(config_.node_count, 0.0);
        for (const auto& bucket : previous_buckets) {
            auto locations = ReadLocations(bucket.bucket_id);
            if (add_candidate_replica && bucket.bucket_id == bucket_id &&
                std::find(locations.begin(), locations.end(), target_node) == locations.end()) {
                locations.push_back(target_node);
            }
            const double logical_gets = bucket.bucket_id == bucket_id
                                            ? predicted_gets
                                            : static_cast<double>(bucket.get_count);
            const auto routed_gets = static_cast<std::size_t>(std::llround(logical_gets));
            for (std::size_t request = 0; request < routed_gets; ++request) {
                const auto node = *std::min_element(
                    locations.begin(), locations.end(),
                    [&node_operations](std::size_t left, std::size_t right) {
                        return node_operations[left] < node_operations[right];
                    });
                node_operations[node] += 1.0;
            }
            for (const auto node : locations) {
                node_operations[node] += static_cast<double>(bucket.put_count);
            }
        }
        return EstimateWindowCompletionMs(node_operations);
    };

    return std::max(
        0.0,
        estimate_completion(false) - estimate_completion(true));
}

void ClusterSimulator::SetBucketBytes(std::vector<std::size_t> bucket_bytes) {
    if (bucket_bytes.size() != config_.bucket_count) {
        throw std::invalid_argument("bucket byte vector has the wrong size");
    }
    bucket_bytes_ = std::move(bucket_bytes);
}

const SimulationConfig& ClusterSimulator::Config() const {
    return config_;
}

std::vector<std::size_t> ClusterSimulator::ReadLocations(std::size_t bucket_id) const {
    std::vector<std::size_t> locations{HomeNode(bucket_id)};
    for (const auto& replica : replicas_.at(bucket_id)) {
        locations.push_back(replica.node_id);
    }
    return locations;
}

}  // namespace hotbucket
