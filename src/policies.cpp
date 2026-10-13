#include "hotbucket/policies.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

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

std::vector<std::string> SplitCsvRow(const std::string& row) {
    std::vector<std::string> fields;
    std::stringstream stream(row);
    std::string field;
    while (std::getline(stream, field, ',')) {
        if (!field.empty() && field.back() == '\r') {
            field.pop_back();
        }
        if (fields.empty() && field.size() >= 3 &&
            static_cast<unsigned char>(field[0]) == 0xefU &&
            static_cast<unsigned char>(field[1]) == 0xbbU &&
            static_cast<unsigned char>(field[2]) == 0xbfU) {
            field.erase(0, 3);
        }
        fields.push_back(field);
    }
    return fields;
}

std::size_t ColumnIndex(const std::vector<std::string>& header, const std::string& name) {
    const auto found = std::find(header.begin(), header.end(), name);
    if (found == header.end()) {
        throw std::invalid_argument("prediction CSV is missing column: " + name);
    }
    return static_cast<std::size_t>(std::distance(header.begin(), found));
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
        cluster.EstimateCopyCostMs(candidate->bucket_id),
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

    const auto target = cluster.LeastLoadedAlternative(bucket, previous.node_operations);
    if (target == cluster.HomeNode(bucket)) {
        return std::nullopt;
    }
    const double expected_benefit_ms = cluster.EstimateReplicationBenefitMs(
        bucket, predicted_gets, target, previous.buckets);
    const double expected_cost_ms = cluster.EstimateCopyCostMs(bucket);
    if (expected_benefit_ms <= expected_cost_ms) {
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

CostAwareLstmPolicy::CostAwareLstmPolicy(
    std::size_t severe_get_threshold,
    double minimum_probability,
    double minimum_predicted_get_ratio,
    std::vector<LstmBucketPrediction> predictions)
    : severe_get_threshold_(severe_get_threshold),
      minimum_probability_(minimum_probability),
      minimum_predicted_get_ratio_(minimum_predicted_get_ratio) {
    if (minimum_probability_ < 0.0 || minimum_probability_ > 1.0) {
        throw std::invalid_argument("minimum LSTM probability must be between 0 and 1");
    }
    if (minimum_predicted_get_ratio_ < 0.0 || minimum_predicted_get_ratio_ > 1.0) {
        throw std::invalid_argument(
            "minimum predicted GET ratio must be between 0 and 1");
    }
    for (const auto& prediction : predictions) {
        const auto key = std::make_pair(prediction.window_id, prediction.bucket_id);
        if (!predictions_.emplace(key, prediction).second) {
            throw std::invalid_argument("duplicate LSTM prediction for one window and bucket");
        }
    }
}

std::string CostAwareLstmPolicy::Name() const {
    return "lstm";
}

std::optional<ReplicationDecision> CostAwareLstmPolicy::Decide(
    std::size_t window_id,
    const std::vector<WindowResult>& history,
    const ClusterSimulator& cluster) const {
    if (history.empty()) {
        return std::nullopt;
    }

    const auto& previous = history.back();
    const auto first = predictions_.lower_bound(std::make_pair(window_id, std::size_t{0}));
    const LstmBucketPrediction* selected = nullptr;
    double selected_benefit_ms = 0.0;
    double selected_cost_ms = 0.0;
    double selected_utility_ms = 0.0;
    for (auto prediction = first;
         prediction != predictions_.end() && prediction->first.first == window_id;
         ++prediction) {
        const auto& candidate = prediction->second;
        if (candidate.bucket_id >= cluster.Config().bucket_count ||
            cluster.HasReplica(candidate.bucket_id) ||
            candidate.hotspot_probability < minimum_probability_ ||
            candidate.predicted_gets <
                static_cast<double>(severe_get_threshold_) * minimum_predicted_get_ratio_) {
            continue;
        }

        const auto target = cluster.LeastLoadedAlternative(
            candidate.bucket_id, previous.node_operations);
        if (target == cluster.HomeNode(candidate.bucket_id)) {
            continue;
        }
        const double gross_benefit_ms = cluster.EstimateReplicationBenefitMs(
            candidate.bucket_id,
            candidate.predicted_gets,
            target,
            previous.buckets);
        const double confidence_adjusted_benefit_ms =
            candidate.hotspot_probability * gross_benefit_ms;
        const double expected_cost_ms = cluster.EstimateCopyCostMs(candidate.bucket_id);
        const double utility_ms = confidence_adjusted_benefit_ms - expected_cost_ms;
        if (utility_ms > 0.0 &&
            (selected == nullptr || utility_ms > selected_utility_ms)) {
            selected = &candidate;
            selected_benefit_ms = confidence_adjusted_benefit_ms;
            selected_cost_ms = expected_cost_ms;
            selected_utility_ms = utility_ms;
        }
    }
    if (selected == nullptr) {
        return std::nullopt;
    }

    const auto target = cluster.LeastLoadedAlternative(
        selected->bucket_id, previous.node_operations);
    if (target == cluster.HomeNode(selected->bucket_id)) {
        return std::nullopt;
    }
    return ReplicationDecision{
        selected->bucket_id,
        target,
        selected->predicted_gets,
        selected_benefit_ms,
        selected_cost_ms,
        "LSTM confidence-adjusted benefit exceeds copy cost",
        selected->hotspot_probability,
    };
}

std::vector<LstmBucketPrediction> LoadLstmPredictionsCsv(
    const std::string& path,
    const std::string& trace_id) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open LSTM prediction file: " + path);
    }
    std::string line;
    if (!std::getline(input, line)) {
        throw std::invalid_argument("LSTM prediction file is empty: " + path);
    }
    const auto header = SplitCsvRow(line);
    const auto trace_column = ColumnIndex(header, "trace_id");
    const auto window_column = ColumnIndex(header, "window_id");
    const auto bucket_column = ColumnIndex(header, "bucket_id");
    const auto probability_column = ColumnIndex(header, "hotspot_probability");
    const auto gets_column = ColumnIndex(header, "predicted_gets");
    const auto required_size = 1 + std::max(
        {trace_column, window_column, bucket_column, probability_column, gets_column});

    std::vector<LstmBucketPrediction> predictions;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = SplitCsvRow(line);
        if (fields.size() < required_size) {
            throw std::invalid_argument("malformed row in LSTM prediction file");
        }
        if (fields[trace_column] != trace_id) {
            continue;
        }
        predictions.push_back(LstmBucketPrediction{
            std::stoull(fields[window_column]),
            std::stoull(fields[bucket_column]),
            std::stod(fields[probability_column]),
            std::stod(fields[gets_column]),
        });
    }
    if (predictions.empty()) {
        throw std::invalid_argument("no LSTM predictions found for trace: " + trace_id);
    }
    return predictions;
}

std::unique_ptr<ReplicationPolicy> MakePolicy(const std::string& name,
                                              std::size_t severe_get_threshold,
                                              std::vector<LstmBucketPrediction> lstm_predictions,
                                              double lstm_minimum_probability,
                                              double lstm_minimum_predicted_get_ratio) {
    if (name == "no-action") {
        return std::make_unique<NoActionPolicy>();
    }
    if (name == "reactive") {
        return std::make_unique<ReactivePolicy>(severe_get_threshold);
    }
    if (name == "recent-window") {
        return std::make_unique<CostAwareRecentWindowPolicy>(severe_get_threshold);
    }
    if (name == "lstm") {
        return std::make_unique<CostAwareLstmPolicy>(
            severe_get_threshold,
            lstm_minimum_probability,
            lstm_minimum_predicted_get_ratio,
            std::move(lstm_predictions));
    }
    throw std::invalid_argument("unknown policy: " + name);
}

}  // namespace hotbucket
