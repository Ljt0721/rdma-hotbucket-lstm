#include "hotbucket/cluster.hpp"
#include "hotbucket/policies.hpp"
#include "hotbucket/workload.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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
    bool stream_json{false};
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
        } else if (argument == "--stream-json") {
            command.stream_json = true;
        } else if (argument == "--help") {
            std::cout
                << "Usage: hotbucket_sim [options]\n"
                << "  --policy no-action|reactive|recent-window\n"
                << "  --nodes N --buckets N --windows N --requests N --node-capacity N\n"
                << "  --hotspot-duration N --replica-ttl N --threshold N --seed N\n"
                << "  --read-ratio R --hotspot-share R --base-latency-us R\n"
                << "  --bucket-size-mib R --copy-bandwidth-mib-per-ms R\n"
                << "  --metadata-cost-ms R\n"
                << "  --stream-json --window-delay-ms N\n"
                << "  --output path/to/result.csv\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }
    return command;
}

void WriteHeader(std::ofstream& output) {
    output << "policy,window_id,bucket_id,home_node,is_generated_hot_bucket,get_count,put_count,"
              "read_ratio,replica_count,mean_latency_us,p99_latency_us,home_node_load_ratio,"
              "cluster_max_load_ratio,window_completion_ms,copy_applied,copy_cost_ms,"
              "decision_bucket,predicted_gets,expected_benefit_ms,expected_cost_ms\n";
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
               << (decision_for_bucket ? result.copy_cost_ms : 0.0) << ',';

        if (decision_for_bucket) {
            output << result.decision->bucket_id << ',' << result.decision->predicted_gets << ','
                   << result.decision->expected_benefit_ms << ','
                   << result.decision->expected_cost_ms;
        } else {
            output << ",,,";
        }
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
              << ",\"copies\":" << copies << ",\"decision\":";

    if (result.decision.has_value()) {
        const auto& decision = *result.decision;
        std::cout << "{\"bucketId\":" << decision.bucket_id
                  << ",\"targetNode\":" << decision.target_node
                  << ",\"predictedGets\":" << decision.predicted_gets
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
        auto policy = hotbucket::MakePolicy(command.policy, command.severe_get_threshold);
        hotbucket::MovingHotspotWorkload workload(command.config);
        hotbucket::ClusterSimulator cluster(command.config);
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
            cluster.BeginWindow(window);
            const auto decision = policy->Decide(window, history, cluster);
            double copy_cost_ms = 0.0;
            bool copy_applied = false;
            if (decision.has_value()) {
                copy_applied = cluster.ApplyReplication(*decision, window, &copy_cost_ms);
            }

            const auto hot_bucket = workload.HotBucketForWindow(window);
            const auto requests = workload.GenerateWindow(window);
            auto result = cluster.ProcessWindow(window, hot_bucket, requests, copy_cost_ms);
            result.decision = decision;
            result.copy_applied = copy_applied;
            WriteWindow(output, policy->Name(), result);

            total_completion_ms += result.window_completion_ms;
            max_load_ratio = std::max(max_load_ratio, result.cluster_max_load_ratio);
            copies += copy_applied ? 1 : 0;

            if (command.stream_json) {
                StreamWindowJson(policy->Name(), command.config, result, total_completion_ms, copies);
                if (command.window_delay_ms > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(command.window_delay_ms));
                }
            }
            history.push_back(std::move(result));
        }

        if (command.stream_json) {
            StreamCompleteJson(policy->Name(), command.config.window_count, total_completion_ms,
                               max_load_ratio, copies, output_path);
        } else {
            std::cout << "policy=" << policy->Name() << '\n'
                      << "windows=" << command.config.window_count << '\n'
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
