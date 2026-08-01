#include "hotbucket/policies.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hotbucket {

namespace {

const BucketWindowMetrics* HottestUnreplicatedBucket(const WindowResult& result,
                                                      const ClusterSimulator& cluster) {
    const BucketWindowMetrics* selected = nullptr;
    for (const auto& bucket : result.buckets) {
        if (cluster.HasReplica(bucket.bucket_id)) {
            continue;
        }
        if (selected == nullptr || bucket.get_count > selected->get_count) {
            selected = &bucket;
        }
    }
    return selected;
}

}  // namespace

std::string NoActionPolicy::Name() const {
    return "no-action";
}

std::optional<ReplicationDecision> NoActionPolicy::Decide(
    std::size_t,
    const std::vector<WindowResult>&,
    const ClusterSimulator&) const {
    return std::nullopt;
}

ReactivePolicy::ReactivePolicy(std::size_t severe_get_threshold)
    : severe_get_threshold_(severe_get_threshold) {}

std::string ReactivePolicy::Name() const {
    return "reactive";
}

std::optional<ReplicationDecision> ReactivePolicy::Decide(
    std::size_t,
    const std::vector<WindowResult>& history,
    const ClusterSimulator& cluster) const {
    if (history.empty()) {
        return std::nullopt;
    }

    const auto& previous = history.back();
    const auto* candidate = HottestUnreplicatedBucket(previous, cluster);
    if (candidate == nullptr || candidate->get_count < severe_get_threshold_) {
        return std::nullopt;
    }

    const auto target = cluster.LeastLoadedAlternative(candidate->bucket_id,
                                                       previous.node_operations);
    if (target == cluster.HomeNode(candidate->bucket_id)) {
        return std::nullopt;
    }

    return ReplicationDecision{
        candidate->bucket_id,
        target,
        static_cast<double>(candidate->get_count),
        0.0,
        cluster.EstimateCopyCostMs(),
        "previous window exceeded the severe-hotspot threshold",
    };
}

CostAwareRecentWindowPolicy::CostAwareRecentWindowPolicy(std::size_t severe_get_threshold)
    : severe_get_threshold_(severe_get_threshold) {}

std::string CostAwareRecentWindowPolicy::Name() const {
    return "recent-window";
}

std::optional<ReplicationDecision> CostAwareRecentWindowPolicy::Decide(
    std::size_t,
    const std::vector<WindowResult>& history,
    const ClusterSimulator& cluster) const {
    if (history.size() < 2) {
        return std::nullopt;
    }

    const auto& previous = history.back();
    const auto& older = history[history.size() - 2];
    const auto* candidate = HottestUnreplicatedBucket(previous, cluster);
    if (candidate == nullptr) {
        return std::nullopt;
    }

    const auto bucket = candidate->bucket_id;
    const double recent_gets = static_cast<double>(previous.buckets[bucket].get_count);
    const double older_gets = static_cast<double>(older.buckets[bucket].get_count);
    const double trend = recent_gets - older_gets;
    const double predicted_gets = std::max(0.0, recent_gets + trend);
    if (predicted_gets < static_cast<double>(severe_get_threshold_)) {
        return std::nullopt;
    }

    const auto& config = cluster.Config();
    const double avoidable_requests =
        std::max(0.0, predicted_gets - static_cast<double>(severe_get_threshold_));
    const double expected_benefit_ms =
        avoidable_requests * config.base_latency_us * 4.0 / 1000.0;
    const double expected_cost_ms = cluster.EstimateCopyCostMs();
    if (expected_benefit_ms <= expected_cost_ms) {
        return std::nullopt;
    }

    const auto target = cluster.LeastLoadedAlternative(bucket, previous.node_operations);
    if (target == cluster.HomeNode(bucket)) {
        return std::nullopt;
    }

    return ReplicationDecision{
        bucket,
        target,
        predicted_gets,
        expected_benefit_ms,
        expected_cost_ms,
        "recent-window trend predicts benefit above copy cost",
    };
}

std::unique_ptr<ReplicationPolicy> MakePolicy(const std::string& name,
                                              std::size_t severe_get_threshold) {
    if (name == "no-action") {
        return std::make_unique<NoActionPolicy>();
    }
    if (name == "reactive") {
        return std::make_unique<ReactivePolicy>(severe_get_threshold);
    }
    if (name == "recent-window") {
        return std::make_unique<CostAwareRecentWindowPolicy>(severe_get_threshold);
    }
    throw std::invalid_argument("unknown policy: " + name);
}

}  // namespace hotbucket
