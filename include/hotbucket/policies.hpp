#pragma once

#include "hotbucket/cluster.hpp"
#include "hotbucket/types.hpp"

#include <memory>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hotbucket {

struct LstmBucketPrediction {
    std::size_t window_id{};
    std::size_t bucket_id{};
    double hotspot_probability{};
    double predicted_gets{};
};

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

class CostAwareLstmPolicy final : public ReplicationPolicy {
public:
    CostAwareLstmPolicy(std::size_t severe_get_threshold,
                        double minimum_probability,
                        double minimum_predicted_get_ratio,
                        std::vector<LstmBucketPrediction> predictions);

    std::string Name() const override;
    std::optional<ReplicationDecision> Decide(
        std::size_t window_id,
        const std::vector<WindowResult>& history,
        const ClusterSimulator& cluster) const override;

private:
    std::size_t severe_get_threshold_;
    double minimum_probability_;
    double minimum_predicted_get_ratio_;
    std::map<std::pair<std::size_t, std::size_t>, LstmBucketPrediction> predictions_;
};

std::vector<LstmBucketPrediction> LoadLstmPredictionsCsv(
    const std::string& path,
    const std::string& trace_id);

std::unique_ptr<ReplicationPolicy> MakePolicy(const std::string& name,
                                              std::size_t severe_get_threshold,
                                              std::vector<LstmBucketPrediction> lstm_predictions = {},
                                              double lstm_minimum_probability = 0.70,
                                              double lstm_minimum_predicted_get_ratio = 1.0);

}  // namespace hotbucket
