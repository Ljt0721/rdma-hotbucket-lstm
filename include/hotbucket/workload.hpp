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
    const SimulationConfig& config_;
    std::mt19937_64 random_;
};

}  // namespace hotbucket
