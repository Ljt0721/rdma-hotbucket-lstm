#pragma once

#include "hotbucket/types.hpp"

#include <random>
#include <vector>

namespace hotbucket {

class MovingHotspotWorkload {
public:
    explicit MovingHotspotWorkload(const SimulationConfig& config);

    std::size_t HotBucketForWindow(std::size_t window_id) const;
    std::vector<Request> GenerateWindow(std::size_t window_id);

private:
    struct HotspotMix {
        std::size_t primary_entity_index{};
        std::size_t secondary_entity_index{};
        double primary_share{};
        double secondary_share{};
    };

    std::size_t ScheduledEntityIndex(std::size_t phase) const;
    HotspotMix MixForWindow(std::size_t window_id) const;

    const SimulationConfig& config_;
    std::mt19937_64 random_;
};

}  // namespace hotbucket
