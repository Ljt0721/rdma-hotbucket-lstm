#include "hotbucket/workload.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace hotbucket {

MovingHotspotWorkload::MovingHotspotWorkload(const SimulationConfig& config)
    : config_(config), random_(config.seed) {
    if (config_.bucket_count < 2) {
        throw std::invalid_argument("bucket_count must be at least 2");
    }
    if (config_.entity_count < 2) {
        throw std::invalid_argument("entity_count must be at least 2");
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
    const auto& pattern = config_.workload_pattern;
    if (pattern != "step" && pattern != "ramp" && pattern != "gradual" &&
        pattern != "burst" && pattern != "random" && pattern != "stable") {
        throw std::invalid_argument(
            "workload_pattern must be step, ramp, gradual, burst, random, or stable");
    }
}

std::size_t MovingHotspotWorkload::ScheduledEntityIndex(std::size_t phase) const {
    if (config_.workload_pattern == "stable") {
        return 2 % config_.entity_count;
    }
    if (config_.workload_pattern == "random") {
        std::uint64_t value = config_.seed + phase + 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return static_cast<std::size_t>(value % config_.entity_count);
    }
    return (phase * 3 + 2) % config_.entity_count;
}

MovingHotspotWorkload::HotspotMix MovingHotspotWorkload::MixForWindow(
    std::size_t window_id) const {
    const auto duration = config_.hotspot_duration_windows;
    const auto phase = window_id / duration;
    const auto offset = window_id % duration;
    const auto primary = ScheduledEntityIndex(phase);
    const auto next = ScheduledEntityIndex(phase + 1);

    if (config_.workload_pattern == "ramp") {
        const double progress = duration == 1
                                    ? 1.0
                                    : static_cast<double>(offset) /
                                          static_cast<double>(duration - 1);
        const double minimum_share = std::min(0.20, config_.hotspot_share * 0.35);
        return HotspotMix{
            primary,
            primary,
            minimum_share + (config_.hotspot_share - minimum_share) * progress,
            0.0,
        };
    }
    if (config_.workload_pattern == "gradual") {
        const double progress = static_cast<double>(offset) / static_cast<double>(duration);
        return HotspotMix{
            primary,
            next,
            config_.hotspot_share * (1.0 - progress),
            config_.hotspot_share * progress,
        };
    }
    if (config_.workload_pattern == "burst") {
        const bool active_burst = offset < std::max<std::size_t>(1, duration / 3);
        return HotspotMix{
            primary,
            primary,
            active_burst ? config_.hotspot_share : std::min(0.12, config_.hotspot_share),
            0.0,
        };
    }
    return HotspotMix{primary, primary, config_.hotspot_share, 0.0};
}

std::size_t MovingHotspotWorkload::HotBucketForWindow(std::size_t window_id) const {
    const auto mix = MixForWindow(window_id);
    const auto entity_index = mix.secondary_share > mix.primary_share
                                  ? mix.secondary_entity_index
                                  : mix.primary_entity_index;
    return (1001 + entity_index) % config_.bucket_count;
}

std::vector<Request> MovingHotspotWorkload::GenerateWindow(std::size_t window_id) {
    std::bernoulli_distribution choose_get(config_.read_ratio);
    std::uniform_real_distribution<double> choose_share(0.0, 1.0);
    std::uniform_int_distribution<std::size_t> choose_entity(0, config_.entity_count - 1);
    const auto mix = MixForWindow(window_id);

    std::vector<Request> requests;
    requests.reserve(config_.requests_per_window);

    for (std::size_t index = 0; index < config_.requests_per_window; ++index) {
        const double selection = choose_share(random_);
        std::size_t entity_index = 0;
        if (selection < mix.primary_share) {
            entity_index = mix.primary_entity_index;
        } else if (selection < mix.primary_share + mix.secondary_share) {
            entity_index = mix.secondary_entity_index;
        } else {
            entity_index = choose_entity(random_);
            while ((entity_index == mix.primary_entity_index && mix.primary_share > 0.0) ||
                   (entity_index == mix.secondary_entity_index && mix.secondary_share > 0.0)) {
                entity_index = (entity_index + 1) % config_.entity_count;
            }
        }
        const std::uint64_t entity_id = 1001 + entity_index;
        const auto bucket = entity_id % config_.bucket_count;
        requests.push_back(Request{
            entity_id,
            bucket,
            choose_get(random_) ? Operation::Get : Operation::Put,
        });
    }

    return requests;
}

}  // namespace hotbucket
