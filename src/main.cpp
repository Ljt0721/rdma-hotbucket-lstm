#include "hotbucket/cluster.hpp"
#include "hotbucket/entity_memory.hpp"
#include "hotbucket/policies.hpp"
#include "hotbucket/rdma_replication.hpp"
#include "hotbucket/workload.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct CommandLine {
    hotbucket::SimulationConfig config;
    std::string policy{"no-action"};
    std::string output{"results/run.csv"};
    std::size_t severe_get_threshold{1200};
    std::size_t window_delay_ms{0};
    std::size_t node_memory_bytes{2048};
    std::size_t observation_window_seconds{30};
    std::size_t evaluation_start_window{0};
    std::string trace_id{"trace-0"};
    std::string prediction_file;
    double lstm_minimum_probability{0.70};
    double lstm_minimum_predicted_get_ratio{1.0};
    double lstm_inference_cost_ms{0.0};
    std::string rdma_server;
    std::uint16_t rdma_port_base{7600};
    std::size_t rdma_staging_bytes{hotbucket::kDefaultRdmaStagingBytes};
    bool stream_json{false};
    bool placement_only{false};
    bool trace_only{false};
};

std::string RequireValue(int& index, int argc, char** argv) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") + argv[index]);
    }
    return argv[++index];
}

CommandLine ParseArguments(int argc, char** argv) {
    CommandLine command;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--policy") {
            command.policy = RequireValue(index, argc, argv);
        } else if (argument == "--output") {
            command.output = RequireValue(index, argc, argv);
        } else if (argument == "--nodes") {
            command.config.node_count = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--buckets") {
            command.config.bucket_count = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--entities") {
            command.config.entity_count = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--windows") {
            command.config.window_count = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--requests") {
            command.config.requests_per_window = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--node-capacity") {
            command.config.node_capacity_per_window =
                std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--hotspot-duration") {
            command.config.hotspot_duration_windows =
                std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--workload-pattern") {
            command.config.workload_pattern = RequireValue(index, argc, argv);
        } else if (argument == "--replica-ttl") {
            command.config.replica_ttl_windows =
                std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--read-ratio") {
            command.config.read_ratio = std::stod(RequireValue(index, argc, argv));
        } else if (argument == "--hotspot-share") {
            command.config.hotspot_share = std::stod(RequireValue(index, argc, argv));
        } else if (argument == "--base-latency-us") {
            command.config.base_latency_us = std::stod(RequireValue(index, argc, argv));
        } else if (argument == "--bucket-size-mib") {
            command.config.bucket_size_mib = std::stod(RequireValue(index, argc, argv));
        } else if (argument == "--copy-bandwidth-mib-per-ms") {
            command.config.copy_bandwidth_mib_per_ms =
                std::stod(RequireValue(index, argc, argv));
        } else if (argument == "--metadata-cost-ms") {
            command.config.metadata_cost_ms = std::stod(RequireValue(index, argc, argv));
        } else if (argument == "--threshold") {
            command.severe_get_threshold = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--seed") {
            command.config.seed = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--window-delay-ms") {
            command.window_delay_ms = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--node-memory-bytes") {
            command.node_memory_bytes = std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--window-seconds") {
            command.observation_window_seconds =
                std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--trace-id") {
            command.trace_id = RequireValue(index, argc, argv);
        } else if (argument == "--prediction-file") {
            command.prediction_file = RequireValue(index, argc, argv);
        } else if (argument == "--lstm-min-probability") {
            command.lstm_minimum_probability = std::stod(RequireValue(index, argc, argv));
        } else if (argument == "--lstm-min-predicted-get-ratio") {
            command.lstm_minimum_predicted_get_ratio =
                std::stod(RequireValue(index, argc, argv));
        } else if (argument == "--lstm-inference-cost-ms") {
            command.lstm_inference_cost_ms = std::stod(RequireValue(index, argc, argv));
        } else if (argument == "--evaluation-start-window") {
            command.evaluation_start_window =
                std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--rdma-server") {
            command.rdma_server = RequireValue(index, argc, argv);
        } else if (argument == "--rdma-port-base") {
            const auto port = std::stoull(RequireValue(index, argc, argv));
            if (port == 0 || port > 65535) {
                throw std::invalid_argument("rdma-port-base must be between 1 and 65535");
            }
            command.rdma_port_base = static_cast<std::uint16_t>(port);
        } else if (argument == "--rdma-staging-bytes") {
            command.rdma_staging_bytes =
                std::stoull(RequireValue(index, argc, argv));
        } else if (argument == "--stream-json") {
            command.stream_json = true;
        } else if (argument == "--placement-only") {
            command.placement_only = true;
        } else if (argument == "--trace-only") {
            command.trace_only = true;
        } else if (argument == "--help") {
            std::cout
                << "Usage: hotbucket_sim [options]\n"
                << "  --policy no-action|reactive|recent-window|lstm\n"
                << "  --nodes N --buckets N --entities N --windows N --requests N\n"
                << "  --node-capacity N --node-memory-bytes N\n"
                << "  --hotspot-duration N --replica-ttl N --threshold N --seed N\n"
                << "  --workload-pattern step|ramp|gradual|burst|random|stable\n"
                << "  --read-ratio R --hotspot-share R --base-latency-us R\n"
                << "  --bucket-size-mib R --copy-bandwidth-mib-per-ms R\n"
                << "  --metadata-cost-ms R\n"
                << "  --stream-json --placement-only --trace-only --trace-id ID\n"
                << "  --prediction-file path.csv --lstm-min-probability R\n"
                << "  --lstm-min-predicted-get-ratio R\n"
                << "  --lstm-inference-cost-ms R\n"
                << "  --evaluation-start-window N\n"
                << "  --rdma-server IPv4 --rdma-port-base N --rdma-staging-bytes N\n"
                << "  --window-seconds N --window-delay-ms N\n"
                << "  --output path/to/result.csv\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }
    return command;
}

void WriteTrainingTrace(const CommandLine& command,
                        const std::vector<hotbucket::BattlefieldEntity>& entities,
                        const hotbucket::EntityMemoryCluster& memory_cluster) {
    if (command.observation_window_seconds == 0) {
        throw std::invalid_argument("window_seconds must be positive");
    }

    const std::filesystem::path output_path(command.output);
    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output(output_path);
    if (!output) {
        throw std::runtime_error("cannot open trace output file: " + command.output);
    }
    output << std::fixed << std::setprecision(6);
    output << "trace_id,pattern,seed,window_id,window_seconds,bucket_id,home_node,"
              "bucket_bytes,requests_per_window,node_capacity,hotspot_duration,"
              "severe_get_threshold,configured_read_ratio,configured_hotspot_share,"
              "get_count,put_count,"
              "read_ratio,request_share,home_node_logical_ops,"
              "home_node_logical_load_ratio,generated_hot_bucket\n";

    std::vector<bool> active_buckets(command.config.bucket_count, false);
    for (const auto& entity : entities) {
        active_buckets[entity.entity_id % command.config.bucket_count] = true;
    }

    hotbucket::MovingHotspotWorkload workload(command.config);
    for (std::size_t window = 0; window < command.config.window_count; ++window) {
        const auto requests = workload.GenerateWindow(window);
        const auto generated_hot_bucket = workload.HotBucketForWindow(window);
        std::vector<std::size_t> gets(command.config.bucket_count, 0);
        std::vector<std::size_t> puts(command.config.bucket_count, 0);
        std::vector<std::size_t> node_operations(command.config.node_count, 0);
        for (const auto& request : requests) {
            if (request.operation == hotbucket::Operation::Get) {
                ++gets[request.bucket_id];
            } else {
                ++puts[request.bucket_id];
            }
            ++node_operations[request.bucket_id % command.config.node_count];
        }

        for (std::size_t bucket = 0; bucket < command.config.bucket_count; ++bucket) {
            if (!active_buckets[bucket]) {
                continue;
            }
            const auto logical_requests = gets[bucket] + puts[bucket];
            const auto home_node = bucket % command.config.node_count;
            const double read_ratio = logical_requests == 0
                                          ? 0.0
                                          : static_cast<double>(gets[bucket]) /
                                                static_cast<double>(logical_requests);
            const double request_share = static_cast<double>(logical_requests) /
                                         static_cast<double>(command.config.requests_per_window);
            const double home_load = static_cast<double>(node_operations[home_node]) /
                                     static_cast<double>(
                                         command.config.node_capacity_per_window);
            output << command.trace_id << ',' << command.config.workload_pattern << ','
                   << command.config.seed << ',' << window << ','
                   << command.observation_window_seconds << ',' << bucket << ','
                   << home_node << ',' << memory_cluster.PrimaryBytesInBucket(bucket) << ','
                   << command.config.requests_per_window << ','
                   << command.config.node_capacity_per_window << ','
                   << command.config.hotspot_duration_windows << ','
                   << command.severe_get_threshold << ',' << command.config.read_ratio << ','
                   << command.config.hotspot_share << ','
                   << gets[bucket] << ',' << puts[bucket] << ',' << read_ratio << ','
                   << request_share << ',' << node_operations[home_node] << ',' << home_load
                   << ',' << generated_hot_bucket << '\n';
        }
    }

    std::cout << "trace_id=" << command.trace_id << '\n'
              << "pattern=" << command.config.workload_pattern << '\n'
              << "windows=" << command.config.window_count << '\n'
              << "entities=" << command.config.entity_count << '\n'
              << "csv=" << output_path.string() << '\n';
}

void WriteHeader(std::ofstream& output) {
    output << "policy,window_id,bucket_id,home_node,is_generated_hot_bucket,get_count,put_count,"
              "read_ratio,replica_count,mean_latency_us,p99_latency_us,home_node_load_ratio,"
              "cluster_max_load_ratio,window_completion_ms,copy_applied,copy_cost_ms,"
              "control_overhead_ms,"
              "decision_bucket,predicted_gets,hotspot_probability,"
              "expected_benefit_ms,expected_cost_ms,"
              "rdma_copy,copied_bytes,rdma_sequence,rdma_write_ms,rdma_end_to_end_ms\n";
}

void WriteWindow(std::ofstream& output,
                 const std::string& policy,
                 const hotbucket::WindowResult& result) {
    for (const auto& bucket : result.buckets) {
        const bool decision_for_bucket = result.decision.has_value() &&
                                         result.decision->bucket_id == bucket.bucket_id;
        const auto logical_requests = bucket.get_count + bucket.put_count;
        const double read_ratio = logical_requests == 0
                                      ? 0.0
                                      : static_cast<double>(bucket.get_count) /
                                            static_cast<double>(logical_requests);
        output << policy << ',' << result.window_id << ',' << bucket.bucket_id << ','
               << bucket.home_node << ',' << (bucket.generated_hot_bucket ? 1 : 0) << ','
               << bucket.get_count << ',' << bucket.put_count << ',' << read_ratio << ','
               << bucket.replica_count << ',' << bucket.mean_latency_us << ','
               << bucket.p99_latency_us << ',' << bucket.home_node_load_ratio << ','
               << result.cluster_max_load_ratio << ',' << result.window_completion_ms << ','
               << (decision_for_bucket && result.copy_applied ? 1 : 0) << ','
               << (decision_for_bucket ? result.copy_cost_ms : 0.0) << ','
               << result.control_overhead_ms << ',';

        if (decision_for_bucket) {
            output << result.decision->bucket_id << ',' << result.decision->predicted_gets << ','
                   << result.decision->hotspot_probability << ','
                   << result.decision->expected_benefit_ms << ','
                   << result.decision->expected_cost_ms;
        } else {
            output << ",,,,";
        }
        output << ',' << (decision_for_bucket && result.rdma_copy ? 1 : 0) << ','
               << (decision_for_bucket ? result.copied_bytes : 0) << ','
               << (decision_for_bucket ? result.rdma_sequence : 0) << ','
               << (decision_for_bucket ? result.rdma_write_ms : 0.0) << ','
               << (decision_for_bucket ? result.rdma_end_to_end_ms : 0.0);
        output << '\n';
    }
}

std::string JsonEscape(const std::string& value) {
    std::ostringstream escaped;
    for (const char character : value) {
        switch (character) {
            case '\\':
                escaped << "\\\\";
                break;
            case '"':
                escaped << "\\\"";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                escaped << character;
                break;
        }
    }
    return escaped.str();
}

void StreamPlacementJson(
    const hotbucket::MemoryPlacementSnapshot& placement,
    const std::string& event_type = "placement",
    const std::optional<std::size_t>& window_id = std::nullopt,
    const std::optional<hotbucket::ReplicationDecision>& copied = std::nullopt,
    const std::vector<hotbucket::ReplicaLocation>& expired = {}) {
    std::cout << "{\"type\":\"" << JsonEscape(event_type) << "\"";
    if (window_id.has_value()) {
        std::cout << ",\"windowId\":" << *window_id;
    }
    std::cout << ",\"copiedBucket\":";
    if (copied.has_value()) {
        std::cout << copied->bucket_id << ",\"copiedToNode\":" << copied->target_node;
    } else {
        std::cout << "null,\"copiedToNode\":null";
    }
    std::cout << ",\"expiredReplicas\":[";
    for (std::size_t index = 0; index < expired.size(); ++index) {
        if (index > 0) {
            std::cout << ',';
        }
        std::cout << "{\"bucketId\":" << expired[index].bucket_id
                  << ",\"nodeId\":" << expired[index].node_id << '}';
    }
    std::cout << "],\"alignmentBytes\":"
              << placement.alignment_bytes
              << ",\"bucketCount\":" << placement.bucket_count
              << ",\"nodeCount\":" << placement.nodes.size()
              << ",\"totalCapacityBytes\":" << placement.total_capacity_bytes
              << ",\"totalUsedBytes\":" << placement.total_used_bytes
              << ",\"nodes\":[";

    std::size_t primary_count = 0;
    std::size_t replica_count = 0;
    for (std::size_t node_index = 0; node_index < placement.nodes.size(); ++node_index) {
        if (node_index > 0) {
            std::cout << ',';
        }
        const auto& node = placement.nodes[node_index];
        std::cout << "{\"id\":" << node.node_id
                  << ",\"capacityBytes\":" << node.capacity_bytes
                  << ",\"usedBytes\":" << node.used_bytes
                  << ",\"entities\":[";
        for (std::size_t entity_index = 0; entity_index < node.entities.size(); ++entity_index) {
            if (entity_index > 0) {
                std::cout << ',';
            }
            const auto& allocation = node.entities[entity_index];
            if (allocation.is_replica) {
                ++replica_count;
            } else {
                ++primary_count;
            }
            std::cout << "{\"id\":" << allocation.entity.entity_id
                      << ",\"callsign\":\"" << JsonEscape(allocation.entity.callsign)
                      << "\",\"entityType\":\"" << JsonEscape(allocation.entity.entity_type)
                      << "\",\"bucketId\":" << allocation.bucket_id
                      << ",\"homeNode\":" << allocation.home_node
                      << ",\"nodeId\":" << allocation.node_id
                      << ",\"offsetBytes\":" << allocation.offset_bytes
                      << ",\"serializedBytes\":" << allocation.serialized_bytes
                      << ",\"allocatedBytes\":" << allocation.allocated_bytes
                      << ",\"isReplica\":" << (allocation.is_replica ? "true" : "false")
                      << ",\"expiresAfterWindow\":";
            if (allocation.is_replica) {
                std::cout << allocation.expires_after_window;
            } else {
                std::cout << "null";
            }
            std::cout
                      << ",\"attributes\":[";
            for (std::size_t attribute_index = 0;
                 attribute_index < allocation.entity.attributes.size(); ++attribute_index) {
                if (attribute_index > 0) {
                    std::cout << ',';
                }
                const auto& attribute = allocation.entity.attributes[attribute_index];
                std::cout << "{\"name\":\"" << JsonEscape(attribute.name)
                          << "\",\"valueType\":\"" << JsonEscape(attribute.value_type)
                          << "\",\"value\":\"" << JsonEscape(attribute.value)
                          << "\",\"valueBytes\":" << attribute.value.size() << '}';
            }
            std::cout << "]}";
        }
        std::cout << "]}";
    }
    std::cout << "],\"entityCount\":" << primary_count
              << ",\"primaryCount\":" << primary_count
              << ",\"replicaCount\":" << replica_count << "}" << std::endl;
}

void StreamWindowJson(const std::string& policy,
                      const hotbucket::SimulationConfig& config,
                      const hotbucket::WindowResult& result,
                      double total_completion_ms,
                      std::size_t copies) {
    std::cout << std::fixed << std::setprecision(4)
              << "{\"type\":\"window\",\"policy\":\"" << JsonEscape(policy)
              << "\",\"windowId\":" << result.window_id
              << ",\"windowCount\":" << config.window_count
              << ",\"hotBucket\":" << result.generated_hot_bucket
              << ",\"maxLoadRatio\":" << result.cluster_max_load_ratio
              << ",\"windowCompletionMs\":" << result.window_completion_ms
              << ",\"totalCompletionMs\":" << total_completion_ms
              << ",\"copyApplied\":" << (result.copy_applied ? "true" : "false")
              << ",\"copyCostMs\":" << result.copy_cost_ms
              << ",\"rdmaCopy\":" << (result.rdma_copy ? "true" : "false")
              << ",\"copiedBytes\":" << result.copied_bytes
              << ",\"rdmaSequence\":" << result.rdma_sequence
              << ",\"rdmaWriteMs\":" << result.rdma_write_ms
              << ",\"rdmaEndToEndMs\":" << result.rdma_end_to_end_ms
              << ",\"controlOverheadMs\":" << result.control_overhead_ms
              << ",\"copies\":" << copies << ",\"decision\":";

    if (result.decision.has_value()) {
        const auto& decision = *result.decision;
        std::cout << "{\"bucketId\":" << decision.bucket_id
                  << ",\"targetNode\":" << decision.target_node
                  << ",\"predictedGets\":" << decision.predicted_gets
                  << ",\"hotspotProbability\":" << decision.hotspot_probability
                  << ",\"expectedBenefitMs\":" << decision.expected_benefit_ms
                  << ",\"expectedCostMs\":" << decision.expected_cost_ms
                  << ",\"reason\":\"" << JsonEscape(decision.reason) << "\"}";
    } else {
        std::cout << "null";
    }

    std::cout << ",\"nodes\":[";
    for (std::size_t node = 0; node < result.node_operations.size(); ++node) {
        if (node > 0) {
            std::cout << ',';
        }
        const double load_ratio = static_cast<double>(result.node_operations[node]) /
                                  static_cast<double>(config.node_capacity_per_window);
        std::cout << "{\"id\":" << node << ",\"operations\":"
                  << result.node_operations[node] << ",\"loadRatio\":" << load_ratio << '}';
    }

    std::cout << "],\"buckets\":[";
    for (std::size_t index = 0; index < result.buckets.size(); ++index) {
        if (index > 0) {
            std::cout << ',';
        }
        const auto& bucket = result.buckets[index];
        std::cout << "{\"id\":" << bucket.bucket_id
                  << ",\"homeNode\":" << bucket.home_node
                  << ",\"gets\":" << bucket.get_count
                  << ",\"puts\":" << bucket.put_count
                  << ",\"replicas\":" << bucket.replica_count
                  << ",\"meanLatencyUs\":" << bucket.mean_latency_us
                  << ",\"p99LatencyUs\":" << bucket.p99_latency_us
                  << ",\"homeLoadRatio\":" << bucket.home_node_load_ratio
                  << ",\"isHot\":" << (bucket.generated_hot_bucket ? "true" : "false") << '}';
    }
    std::cout << "]}" << std::endl;
}

void StreamCompleteJson(const std::string& policy,
                        std::size_t windows,
                        double total_completion_ms,
                        double max_load_ratio,
                        std::size_t copies,
                        const std::filesystem::path& output_path) {
    std::cout << std::fixed << std::setprecision(4)
              << "{\"type\":\"complete\",\"policy\":\"" << JsonEscape(policy)
              << "\",\"windows\":" << windows
              << ",\"totalCompletionMs\":" << total_completion_ms
              << ",\"maxLoadRatio\":" << max_load_ratio
              << ",\"copies\":" << copies
              << ",\"csv\":\"" << JsonEscape(output_path.string()) << "\"}" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto command = ParseArguments(argc, argv);
        const auto entities = hotbucket::MakeBattlefieldScenario(command.config.entity_count);
        hotbucket::EntityMemoryCluster memory_cluster(
            command.config.node_count,
            command.config.bucket_count,
            command.node_memory_bytes);
        const auto placement = memory_cluster.Allocate(entities);
        if (command.stream_json) {
            StreamPlacementJson(placement);
        }
        if (command.trace_only) {
            WriteTrainingTrace(command, entities, memory_cluster);
            return 0;
        }
        if (command.placement_only) {
            if (!command.stream_json) {
                std::cout << "entities=" << command.config.entity_count << '\n'
                          << "nodes=" << command.config.node_count << '\n'
                          << "used_bytes=" << placement.total_used_bytes << '\n';
            }
            return 0;
        }

        if (command.evaluation_start_window >= command.config.window_count) {
            throw std::invalid_argument(
                "evaluation_start_window must be smaller than window_count");
        }
        if (command.lstm_inference_cost_ms < 0.0) {
            throw std::invalid_argument("lstm_inference_cost_ms cannot be negative");
        }
        if (!command.rdma_server.empty()) {
#ifndef HOTBUCKET_HAS_RDMA
            throw std::invalid_argument(
                "this executable was built without RDMA support; use hotbucket_rdma_sim");
#else
            const auto highest_port = static_cast<std::size_t>(command.rdma_port_base) +
                                      command.config.node_count - 1;
            if (highest_port > 65535) {
                throw std::invalid_argument(
                    "rdma-port-base plus the highest node ID exceeds 65535");
            }
            if (command.rdma_staging_bytes == 0 ||
                command.rdma_staging_bytes >
                    std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument(
                    "rdma-staging-bytes is outside the protocol limit");
            }
#endif
        }
#ifdef HOTBUCKET_HAS_RDMA
        std::vector<std::unique_ptr<hotbucket::RdmaReplicaSession>> rdma_sessions;
        if (!command.rdma_server.empty()) {
            rdma_sessions.reserve(command.config.node_count);
            for (std::size_t node = 0; node < command.config.node_count; ++node) {
                const auto port = static_cast<std::uint16_t>(
                    static_cast<std::size_t>(command.rdma_port_base) + node);
                rdma_sessions.push_back(
                    std::make_unique<hotbucket::RdmaReplicaSession>(
                        command.rdma_server,
                        port,
                        node,
                        command.rdma_staging_bytes));
            }
        }
#endif
        std::vector<hotbucket::LstmBucketPrediction> lstm_predictions;
        if (command.policy == "lstm") {
            if (command.prediction_file.empty()) {
                throw std::invalid_argument("lstm policy requires --prediction-file");
            }
            lstm_predictions = hotbucket::LoadLstmPredictionsCsv(
                command.prediction_file, command.trace_id);
        }
        auto policy = hotbucket::MakePolicy(
            command.policy,
            command.severe_get_threshold,
            std::move(lstm_predictions),
            command.lstm_minimum_probability,
            command.lstm_minimum_predicted_get_ratio);
        hotbucket::MovingHotspotWorkload workload(command.config);
        hotbucket::ClusterSimulator cluster(command.config);
        std::vector<std::size_t> bucket_bytes(command.config.bucket_count, 0);
        for (std::size_t bucket = 0; bucket < command.config.bucket_count; ++bucket) {
            bucket_bytes[bucket] = memory_cluster.PrimaryBytesInBucket(bucket);
        }
        cluster.SetBucketBytes(std::move(bucket_bytes));
        std::vector<hotbucket::WindowResult> history;
        history.reserve(command.config.window_count);

        const std::filesystem::path output_path(command.output);
        if (output_path.has_parent_path()) {
            std::filesystem::create_directories(output_path.parent_path());
        }
        std::ofstream output(output_path);
        if (!output) {
            throw std::runtime_error("cannot open output file: " + command.output);
        }
        output << std::fixed << std::setprecision(4);
        WriteHeader(output);

        double total_completion_ms = 0.0;
        std::size_t copies = 0;
        double max_load_ratio = 0.0;

        for (std::size_t window = 0; window < command.config.window_count; ++window) {
            const auto expired_replicas = cluster.BeginWindow(window);
            memory_cluster.ExpireReplicas(window);
            std::optional<hotbucket::ReplicationDecision> decision;
            if (window >= command.evaluation_start_window) {
                decision = policy->Decide(window, history, cluster);
            }
            double copy_cost_ms = 0.0;
            bool copy_applied = false;
            bool rdma_copy = false;
            std::size_t copied_bytes = 0;
            std::uint64_t rdma_sequence = 0;
            double rdma_write_ms = 0.0;
            double rdma_end_to_end_ms = 0.0;
            if (decision.has_value()) {
                const auto expires_after = window + command.config.replica_ttl_windows;
                if (memory_cluster.CanReplicateBucket(
                        decision->bucket_id, decision->target_node)) {
#ifdef HOTBUCKET_HAS_RDMA
                    double measured_rdma_cost_ms = 0.0;
                    if (!command.rdma_server.empty()) {
                        const auto bundle = memory_cluster.BuildReplicaBundle(
                            decision->bucket_id, decision->target_node, expires_after);
                        const auto encoded = hotbucket::EncodeReplicaBundle(bundle);
                        const auto transfer = rdma_sessions.at(decision->target_node)
                                                  ->Replicate(encoded);
                        measured_rdma_cost_ms = transfer.end_to_end_ms;
                        rdma_copy = true;
                        copied_bytes = transfer.transferred_bytes;
                        rdma_sequence = transfer.sequence_number;
                        rdma_write_ms = transfer.write_completion_ms;
                        rdma_end_to_end_ms = transfer.end_to_end_ms;
                    }
#endif
                    if (!memory_cluster.ReplicateBucket(
                            decision->bucket_id, decision->target_node, expires_after)) {
                        throw std::logic_error(
                            "replica became invalid after its transport completed");
                    }
                    copy_applied = cluster.ApplyReplication(*decision, window, &copy_cost_ms);
                    if (!copy_applied) {
                        throw std::logic_error(
                            "memory copy succeeded but routing replica was rejected");
                    }
#ifdef HOTBUCKET_HAS_RDMA
                    if (!command.rdma_server.empty()) {
                        copy_cost_ms = measured_rdma_cost_ms;
                    }
#endif
                }
            }

            const auto hot_bucket = workload.HotBucketForWindow(window);
            const auto requests = workload.GenerateWindow(window);
            const double control_overhead_ms =
                command.policy == "lstm" && window >= command.evaluation_start_window
                    ? command.lstm_inference_cost_ms
                    : 0.0;
            auto result = cluster.ProcessWindow(
                window, hot_bucket, requests, copy_cost_ms, control_overhead_ms);
            result.decision = decision;
            result.copy_applied = copy_applied;
            result.rdma_copy = rdma_copy;
            result.copied_bytes = copied_bytes;
            result.rdma_sequence = rdma_sequence;
            result.rdma_write_ms = rdma_write_ms;
            result.rdma_end_to_end_ms = rdma_end_to_end_ms;
            if (window >= command.evaluation_start_window) {
                WriteWindow(output, policy->Name(), result);

                total_completion_ms += result.window_completion_ms;
                max_load_ratio = std::max(max_load_ratio, result.cluster_max_load_ratio);
                copies += copy_applied ? 1 : 0;

                if (command.stream_json) {
                    StreamWindowJson(
                        policy->Name(), command.config, result, total_completion_ms, copies);
                    StreamPlacementJson(
                        memory_cluster.Snapshot(),
                        "memory",
                        window,
                        copy_applied ? decision : std::nullopt,
                        expired_replicas);
                    if (command.window_delay_ms > 0) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(command.window_delay_ms));
                    }
                }
            }
            history.push_back(std::move(result));
        }

        const auto evaluated_windows =
            command.config.window_count - command.evaluation_start_window;

        if (command.stream_json) {
            StreamCompleteJson(policy->Name(), evaluated_windows, total_completion_ms,
                               max_load_ratio, copies, output_path);
        } else {
            std::cout << "policy=" << policy->Name() << '\n'
                      << "windows=" << evaluated_windows << '\n'
                      << "evaluation_start_window="
                      << command.evaluation_start_window << '\n'
                      << "total_completion_ms=" << std::fixed << std::setprecision(3)
                      << total_completion_ms << '\n'
                      << "max_node_load_ratio=" << max_load_ratio << '\n'
                      << "copies=" << copies << '\n'
                      << "csv=" << output_path.string() << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
