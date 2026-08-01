#include "hotbucket/workload.hpp"

#include <stdexcept>

namespace hotbucket {

MovingHotspotWorkload::MovingHotspotWorkload(const SimulationConfig& config)
    : config_(config), random_(config.seed) {
    if (config_.bucket_count < 2) {
        throw std::invalid_argument("bucket_count must be at least 2");
    }
    if (config_.hotspot_duration_windows == 0) {
        throw std::invalid_argument("hotspot_duration_windows must be positive");
    }
    if (config_.read_ratio < 0.0 || config_.read_ratio > 1.0) {
        throw std::invalid_argument("read_ratio must be between 0 and 1");
    }
    if (config_.hotspot_share < 0.0 || config_.hotspot_share > 1.0) {
        throw std::invalid_argument("hotspot_share must be between 0 and 1");
    }
}

std::size_t MovingHotspotWorkload::HotBucketForWindow(std::size_t window_id) const {
    const auto phase = window_id / config_.hotspot_duration_windows;
    return (phase * 17 + 3) % config_.bucket_count;
}

std::vector<Request> MovingHotspotWorkload::GenerateWindow(std::size_t window_id) {
    const auto hot_bucket = HotBucketForWindow(window_id);
    std::bernoulli_distribution choose_hot(config_.hotspot_share);
    std::bernoulli_distribution choose_get(config_.read_ratio);
    std::uniform_int_distribution<std::size_t> choose_bucket(0, config_.bucket_count - 1);
    std::uniform_int_distribution<std::uint64_t> choose_key_suffix(0, 1'000'000);

    std::vector<Request> requests;
    requests.reserve(config_.requests_per_window);

    for (std::size_t index = 0; index < config_.requests_per_window; ++index) {
        const bool selected_hot_bucket = choose_hot(random_);
        std::size_t bucket = selected_hot_bucket ? hot_bucket : choose_bucket(random_);
        if (!selected_hot_bucket && bucket == hot_bucket) {
            bucket = (bucket + 1) % config_.bucket_count;
        }

        const auto suffix = choose_key_suffix(random_);
        const auto entity_id = suffix * config_.bucket_count + bucket;
        requests.push_back(Request{
            entity_id,
            bucket,
            choose_get(random_) ? Operation::Get : Operation::Put,
        });
    }

    return requests;
}

}  // namespace hotbucket
