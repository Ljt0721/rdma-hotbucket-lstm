#include "hotbucket/cluster.hpp"
#include "hotbucket/entity_memory.hpp"
#include "hotbucket/policies.hpp"
#include "hotbucket/replica_bundle.hpp"
#include "hotbucket/workload.hpp"

#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
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

void TestTenEntitiesAreStoredAcrossFiveMemoryNodes() {
    const auto entities = hotbucket::MakeBattlefieldScenario(10);
    hotbucket::EntityMemoryCluster memory_cluster(5, 64, 2048, 64);
    const auto placement = memory_cluster.Allocate(entities);

    Require(placement.nodes.size() == 5, "placement should contain five memory nodes");
    Require(placement.total_used_bytes > 0, "serialized entities should occupy memory");

    std::size_t allocated_entities = 0;
    for (const auto& node : placement.nodes) {
        Require(node.entities.size() == 2,
                "ten consecutive entity IDs should distribute evenly over five nodes");
        Require(node.used_bytes <= node.capacity_bytes, "node memory pool must not overflow");
        for (const auto& allocation : node.entities) {
            ++allocated_entities;
            Require(allocation.node_id == allocation.bucket_id % 5,
                    "entity should be allocated on its bucket home node");
            Require(allocation.offset_bytes % 64 == 0,
                    "entity allocation should start at a 64-byte boundary");
            Require(allocation.allocated_bytes % 64 == 0,
                    "entity allocation size should be aligned");
            Require(allocation.allocated_bytes >= allocation.serialized_bytes,
                    "allocated memory should contain the serialized entity");
            Require(memory_cluster.ReadSerializedEntity(allocation.entity.entity_id) ==
                        hotbucket::SerializeEntity(allocation.entity),
                    "bytes read from the node pool should match the serialized entity");
        }
    }
    Require(allocated_entities == 10, "all ten battlefield entities should be allocated");
}

void TestWorkloadUsesAllocatedBattlefieldEntities() {
    hotbucket::SimulationConfig config;
    config.entity_count = 10;
    config.bucket_count = 64;
    config.requests_per_window = 500;
    hotbucket::MovingHotspotWorkload workload(config);

    for (const auto& request : workload.GenerateWindow(0)) {
        Require(request.entity_id >= 1001 && request.entity_id <= 1010,
                "workload should only access the allocated demo entities");
        Require(request.bucket_id == request.entity_id % config.bucket_count,
                "request bucket should use the same hash mapping as memory placement");
    }
}

void TestRampWorkloadBuildsLoadGradually() {
    hotbucket::SimulationConfig config;
    config.entity_count = 10;
    config.bucket_count = 64;
    config.requests_per_window = 20000;
    config.hotspot_duration_windows = 6;
    config.hotspot_share = 0.75;
    config.workload_pattern = "ramp";
    hotbucket::MovingHotspotWorkload workload(config);
    const auto hot_bucket = workload.HotBucketForWindow(0);

    const auto first = workload.GenerateWindow(0);
    const auto last = workload.GenerateWindow(5);
    std::size_t first_count = 0;
    std::size_t last_count = 0;
    for (const auto& request : first) {
        first_count += request.bucket_id == hot_bucket ? 1 : 0;
    }
    for (const auto& request : last) {
        last_count += request.bucket_id == hot_bucket ? 1 : 0;
    }

    Require(last_count > first_count * 2,
            "ramp workload should create an observable rising hotspot");
}

void TestRandomScheduleIsReproducible() {
    hotbucket::SimulationConfig config;
    config.entity_count = 10;
    config.bucket_count = 64;
    config.hotspot_duration_windows = 3;
    config.workload_pattern = "random";
    config.seed = 2026;
    hotbucket::MovingHotspotWorkload first(config);
    hotbucket::MovingHotspotWorkload second(config);

    for (std::size_t window = 0; window < 30; ++window) {
        Require(first.HotBucketForWindow(window) == second.HotBucketForWindow(window),
                "random hotspot schedule should be repeatable for the same seed");
    }
}

void TestLstmPolicyUsesProbabilityAndCopyCost() {
    hotbucket::SimulationConfig config;
    config.node_count = 2;
    config.bucket_count = 4;
    config.node_capacity_per_window = 100;
    config.base_latency_us = 5.0;
    config.bucket_size_mib = 0.001;
    hotbucket::ClusterSimulator cluster(config);
    cluster.SetBucketBytes({1024, 1024, 1024, 1024});

    hotbucket::WindowResult previous;
    previous.window_id = 4;
    previous.node_operations = {150, 20};
    previous.buckets.resize(4);
    for (std::size_t bucket = 0; bucket < previous.buckets.size(); ++bucket) {
        previous.buckets[bucket].bucket_id = bucket;
        previous.buckets[bucket].home_node = bucket % config.node_count;
    }
    previous.buckets[0].get_count = 150;
    std::vector<hotbucket::WindowResult> history{previous};
    hotbucket::CostAwareLstmPolicy policy(
        100,
        0.70,
        1.0,
        {hotbucket::LstmBucketPrediction{5, 0, 0.90, 180.0}});
    const auto decision = policy.Decide(5, history, cluster);

    Require(decision.has_value(),
            "high-confidence LSTM benefit should pass the copy-cost rule");
    Require(decision->bucket_id == 0, "LSTM policy should select the predicted bucket");
    Require(decision->target_node == 1, "LSTM policy should select the less-loaded node");
    Require(decision->hotspot_probability == 0.90,
            "decision should preserve model confidence for experiment logs");

    hotbucket::CostAwareLstmPolicy uncertain_policy(
        100,
        0.70,
        1.0,
        {hotbucket::LstmBucketPrediction{5, 0, 0.60, 180.0}});
    Require(!uncertain_policy.Decide(5, history, cluster).has_value(),
            "low-confidence prediction should not trigger replication");

    hotbucket::CostAwareLstmPolicy strict_get_policy(
        100,
        0.70,
        1.0,
        {hotbucket::LstmBucketPrediction{5, 0, 0.95, 90.0}});
    Require(!strict_get_policy.Decide(5, history, cluster).has_value(),
            "strict GET gate should reject an under-threshold forecast");
    hotbucket::CostAwareLstmPolicy relaxed_get_policy(
        100,
        0.70,
        0.85,
        {hotbucket::LstmBucketPrediction{5, 0, 0.95, 90.0}});
    Require(relaxed_get_policy.Decide(5, history, cluster).has_value(),
            "validation-tuned GET ratio should allow a high-confidence near-threshold forecast");
}

void TestControlOverheadIsIncludedInCompletionTime() {
    hotbucket::SimulationConfig config;
    config.node_count = 2;
    config.bucket_count = 4;
    hotbucket::ClusterSimulator cluster(config);
    const auto result = cluster.ProcessWindow(0, 0, {}, 1.25, 0.75);

    Require(std::abs(result.window_completion_ms - 2.0) < 1e-9,
            "completion time should include copy and prediction overhead");
    Require(std::abs(result.copy_cost_ms - 1.25) < 1e-9,
            "copy cost should remain separately observable");
    Require(std::abs(result.control_overhead_ms - 0.75) < 1e-9,
            "prediction overhead should remain separately observable");
}

void TestReplicaCopiesAndReclaimsEntityBytes() {
    const auto entities = hotbucket::MakeBattlefieldScenario(10);
    hotbucket::EntityMemoryCluster memory_cluster(5, 64, 2048, 64);
    const auto initial = memory_cluster.Allocate(entities);
    const auto bucket_id = static_cast<std::size_t>(1003 % 64);
    const auto replica_bytes = memory_cluster.PrimaryBytesInBucket(bucket_id);

    Require(replica_bytes > 0, "hot bucket should contain serialized entity bytes");
    Require(memory_cluster.CanReplicateBucket(bucket_id, 0),
            "target node should have space for the hot bucket copy");
    Require(memory_cluster.ReplicateBucket(bucket_id, 0, 2),
            "replication should allocate bytes on the target memory node");

    const auto replicated = memory_cluster.Snapshot();
    Require(replicated.total_used_bytes == initial.total_used_bytes + replica_bytes,
            "replica should increase physical memory usage by its aligned byte size");
    Require(memory_cluster.ReadSerializedEntity(1003, 0, true) ==
                hotbucket::SerializeEntity(entities[2]),
            "target node bytes should exactly match the primary entity serialization");

    std::size_t replica_count = 0;
    for (const auto& node : replicated.nodes) {
        for (const auto& allocation : node.entities) {
            replica_count += allocation.is_replica ? 1 : 0;
        }
    }
    Require(replica_count == 1, "snapshot should identify one physical replica");
    Require(memory_cluster.ExpireReplicas(2) == 0,
            "replica should remain valid through its final configured window");
    Require(memory_cluster.ExpireReplicas(3) == 1,
            "expired replica should be removed from target memory");
    Require(memory_cluster.Snapshot().total_used_bytes == initial.total_used_bytes,
            "reclaiming a replica should restore initial physical memory usage");
}

void TestReplicaBundleRoundTripAndCorruptionDetection() {
    const auto entities = hotbucket::MakeBattlefieldScenario(10);
    hotbucket::EntityMemoryCluster memory_cluster(5, 64, 2048, 64);
    memory_cluster.Allocate(entities);
    const auto bucket_id = static_cast<std::size_t>(1003 % 64);

    const auto bundle = memory_cluster.BuildReplicaBundle(bucket_id, 0, 7);
    const auto encoded = hotbucket::EncodeReplicaBundle(bundle);
    const auto decoded = hotbucket::DecodeReplicaBundle(encoded);
    Require(decoded.bucket_id == bucket_id, "bundle should preserve its bucket ID");
    Require(decoded.target_node == 0, "bundle should preserve its target node");
    Require(decoded.expires_after_window == 7, "bundle should preserve its expiry window");
    Require(decoded.records.size() == 1, "test bucket should contain one entity record");
    Require(decoded.records[0].entity_id == 1003,
            "bundle should preserve the replicated entity ID");
    Require(decoded.records[0].bytes.size() == decoded.records[0].allocated_bytes,
            "bundle should contain the complete aligned allocation");
    Require(std::vector<std::uint8_t>(decoded.records[0].bytes.begin(),
                                      decoded.records[0].bytes.begin() +
                                          decoded.records[0].serialized_bytes) ==
                hotbucket::SerializeEntity(entities[2]),
            "bundle payload should match the primary entity bytes");

    auto corrupted = encoded;
    corrupted.back() ^= 0x01U;
    bool rejected = false;
    try {
        static_cast<void>(hotbucket::DecodeReplicaBundle(corrupted));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Require(rejected, "checksum should reject a corrupted replica bundle");
}

void TestPredictionCsvAcceptsWindowsLineEndingsOnLinux() {
    const auto path = std::filesystem::temp_directory_path() /
                      "hotbucket-prediction-crlf-test.csv";
    {
        std::ofstream output(path, std::ios::binary);
        output << "trace_id,window_id,bucket_id,hotspot_probability,predicted_gets\r\n"
               << "trace-crlf,8,43,0.75,1400.5\r\n";
    }
    const auto predictions =
        hotbucket::LoadLstmPredictionsCsv(path.string(), "trace-crlf");
    std::filesystem::remove(path);
    Require(predictions.size() == 1, "CRLF prediction file should load one row");
    Require(predictions[0].window_id == 8 && predictions[0].bucket_id == 43,
            "CRLF prediction row should preserve its identifiers");
    Require(std::abs(predictions[0].predicted_gets - 1400.5) < 1e-9,
            "CRLF prediction row should preserve its numeric value");
}

}  // namespace

int main() {
    try {
        TestHotspotMovesDeterministically();
        TestReplicaSplitsReadLoad();
        TestTenEntitiesAreStoredAcrossFiveMemoryNodes();
        TestWorkloadUsesAllocatedBattlefieldEntities();
        TestRampWorkloadBuildsLoadGradually();
        TestRandomScheduleIsReproducible();
        TestLstmPolicyUsesProbabilityAndCopyCost();
        TestControlOverheadIsIncludedInCompletionTime();
        TestReplicaCopiesAndReclaimsEntityBytes();
        TestReplicaBundleRoundTripAndCorruptionDetection();
        TestPredictionCsvAcceptsWindowsLineEndingsOnLinux();
        std::cout << "all tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
