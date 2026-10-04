#include "hotbucket/entity_memory.hpp"
#include "hotbucket/rdma_replication.hpp"
#include "hotbucket/replica_bundle.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string mode;
    std::string server_ip;
    std::uint16_t port{7600};
    std::size_t node_id{0};
    std::size_t maximum_replications{1};
    std::size_t maximum_bundle_bytes{64U * 1024U * 1024U};
    std::size_t bucket_id{43};
    std::size_t target_node{0};
    std::size_t expires_after_window{3};
    std::size_t entity_count{10};
    std::size_t bucket_count{64};
    std::size_t node_count{5};
    std::size_t node_memory_bytes{2048};
};

std::string RequireValue(int& index, int argc, char** argv) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") + argv[index]);
    }
    return argv[++index];
}

unsigned long long ParseUnsigned(const std::string& text, const char* name) {
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument(std::string(name) + " must be a non-negative integer");
    }
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 10);
    if (consumed != text.size()) {
        throw std::invalid_argument(std::string(name) + " must contain digits only");
    }
    return value;
}

std::size_t ParseSize(const std::string& text, const char* name) {
    const auto value = ParseUnsigned(text, name);
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::out_of_range(std::string(name) + " exceeds size_t");
    }
    return static_cast<std::size_t>(value);
}

std::uint16_t ParsePort(const std::string& text) {
    const auto value = ParseUnsigned(text, "port");
    if (value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::out_of_range("port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(value);
}

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  hotbucket_rdma_replica server [options]\n"
        << "  hotbucket_rdma_replica client --server IP [options]\n\n"
        << "Server options:\n"
        << "  --port N --node-id N --max-replications N --max-bundle-bytes N\n"
        << "Client options:\n"
        << "  --server IP --port N --bucket N --target-node N\n"
        << "  --expires-after-window N --entities N --buckets N --nodes N\n"
        << "  --node-memory-bytes N\n";
}

Options ParseArguments(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        throw std::invalid_argument("mode must be server or client");
    }
    Options options;
    options.mode = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--server") {
            options.server_ip = RequireValue(index, argc, argv);
        } else if (argument == "--port") {
            options.port = ParsePort(RequireValue(index, argc, argv));
        } else if (argument == "--node-id") {
            options.node_id = ParseSize(RequireValue(index, argc, argv), "node-id");
        } else if (argument == "--max-replications") {
            options.maximum_replications =
                ParseSize(RequireValue(index, argc, argv), "max-replications");
        } else if (argument == "--max-bundle-bytes") {
            options.maximum_bundle_bytes =
                ParseSize(RequireValue(index, argc, argv), "max-bundle-bytes");
        } else if (argument == "--bucket") {
            options.bucket_id = ParseSize(RequireValue(index, argc, argv), "bucket");
        } else if (argument == "--target-node") {
            options.target_node =
                ParseSize(RequireValue(index, argc, argv), "target-node");
        } else if (argument == "--expires-after-window") {
            options.expires_after_window =
                ParseSize(RequireValue(index, argc, argv), "expires-after-window");
        } else if (argument == "--entities") {
            options.entity_count = ParseSize(RequireValue(index, argc, argv), "entities");
        } else if (argument == "--buckets") {
            options.bucket_count = ParseSize(RequireValue(index, argc, argv), "buckets");
        } else if (argument == "--nodes") {
            options.node_count = ParseSize(RequireValue(index, argc, argv), "nodes");
        } else if (argument == "--node-memory-bytes") {
            options.node_memory_bytes =
                ParseSize(RequireValue(index, argc, argv), "node-memory-bytes");
        } else if (argument == "--help") {
            PrintUsage();
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = ParseArguments(argc, argv);
        if (options.mode == "server") {
            hotbucket::RunRdmaReplicaServer(
                options.port,
                options.node_id,
                options.maximum_replications,
                options.maximum_bundle_bytes);
        } else if (options.mode == "client") {
            if (options.server_ip.empty()) {
                throw std::invalid_argument("client mode requires --server IP");
            }
            const auto entities = hotbucket::MakeBattlefieldScenario(options.entity_count);
            hotbucket::EntityMemoryCluster memory_cluster(
                options.node_count,
                options.bucket_count,
                options.node_memory_bytes);
            memory_cluster.Allocate(entities);
            const auto bundle = memory_cluster.BuildReplicaBundle(
                options.bucket_id,
                options.target_node,
                options.expires_after_window);
            const auto encoded = hotbucket::EncodeReplicaBundle(bundle);
            const auto result = hotbucket::ReplicateBundleRdma(
                options.server_ip, options.port, encoded);
            std::cout << "replication_complete bucket=" << result.bucket_id
                      << " target_node=" << result.target_node
                      << " records=" << bundle.records.size()
                      << " bytes=" << result.transferred_bytes
                      << " write_completion_ms=" << result.write_completion_ms
                      << " end_to_end_ms=" << result.end_to_end_ms
                      << " provider=" << result.provider << '\n';
        } else {
            throw std::invalid_argument("mode must be server or client");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
