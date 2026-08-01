#include "hotbucket/cluster.hpp"
#include "hotbucket/workload.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void TestHotspotMovesDeterministically() {
    hotbucket::SimulationConfig config;
    config.bucket_count = 32;
    config.hotspot_duration_windows = 3;
    hotbucket::MovingHotspotWorkload workload(config);

    Require(workload.HotBucketForWindow(0) == workload.HotBucketForWindow(2),
            "hotspot should stay for the configured duration");
    Require(workload.HotBucketForWindow(2) != workload.HotBucketForWindow(3),
            "hotspot should move at the next phase");
}

void TestReplicaSplitsReadLoad() {
    hotbucket::SimulationConfig config;
    config.node_count = 2;
    config.bucket_count = 4;
    config.node_capacity_per_window = 100;
    config.base_latency_us = 5.0;
    hotbucket::ClusterSimulator cluster(config);

    std::vector<hotbucket::Request> requests;
    for (std::size_t index = 0; index < 120; ++index) {
        requests.push_back(hotbucket::Request{index * 4, 0, hotbucket::Operation::Get});
    }

    cluster.BeginWindow(0);
    const auto before = cluster.ProcessWindow(0, 0, requests, 0.0);
    const hotbucket::ReplicationDecision decision{0, 1, 120.0, 10.0, 1.0, "test"};
    double copy_cost = 0.0;
    Require(cluster.ApplyReplication(decision, 1, &copy_cost),
            "replication should be applied to the alternative node");
    const auto after = cluster.ProcessWindow(1, 0, requests, copy_cost);

    Require(after.cluster_max_load_ratio < before.cluster_max_load_ratio,
            "replica should reduce the maximum node load");
    Require(after.buckets[0].replica_count == 1,
            "replica should appear in bucket metrics");
}

}  // namespace

int main() {
    try {
        TestHotspotMovesDeterministically();
        TestReplicaSplitsReadLoad();
        std::cout << "all tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
