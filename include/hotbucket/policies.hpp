#pragma once

#include "hotbucket/cluster.hpp"
#include "hotbucket/types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hotbucket {

class ReplicationPolicy {
public:
    virtual ~ReplicationPolicy() = default;
    virtual std::string Name() const = 0;
    virtual std::optional<ReplicationDecision> Decide(
        std::size_t window_id,
        const std::vector<WindowResult>& history,
        const ClusterSimulator& cluster) const = 0;
};

class NoActionPolicy final : public ReplicationPolicy {
public:
    std::string Name() const override;
    std::optional<ReplicationDecision> Decide(
        std::size_t window_id,
        const std::vector<WindowResult>& history,
        const ClusterSimulator& cluster) const override;
};

class ReactivePolicy final : public ReplicationPolicy {
public:
    explicit ReactivePolicy(std::size_t severe_get_threshold);

    std::string Name() const override;
    std::optional<ReplicationDecision> Decide(
        std::size_t window_id,
        const std::vector<WindowResult>& history,
        const ClusterSimulator& cluster) const override;

private:
    std::size_t severe_get_threshold_;
};

class CostAwareRecentWindowPolicy final : public ReplicationPolicy {
public:
    explicit CostAwareRecentWindowPolicy(std::size_t severe_get_threshold);

    std::string Name() const override;
    std::optional<ReplicationDecision> Decide(
        std::size_t window_id,
        const std::vector<WindowResult>& history,
        const ClusterSimulator& cluster) const override;

private:
    std::size_t severe_get_threshold_;
};

std::unique_ptr<ReplicationPolicy> MakePolicy(const std::string& name,
                                              std::size_t severe_get_threshold);

}  // namespace hotbucket
