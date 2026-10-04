#include "hotbucket/rdma_replication.hpp"

#include "hotbucket/replica_bundle.hpp"

#include <arpa/inet.h>
#include <endian.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <poll.h>
#include <rdma/rdma_cma.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hotbucket {

namespace {

constexpr int kResolveTimeoutMs = 5000;
constexpr int kCmEventTimeoutMs = 30000;
constexpr int kServerIdleTimeoutMs = 300000;
constexpr int kCompletionTimeoutMs = 60000;
constexpr std::uint32_t kRequestMagic = 0x48425251U;
constexpr std::uint32_t kDoneMagic = 0x48425244U;
constexpr std::uint32_t kAckMagic = 0x48425241U;
constexpr std::uint32_t kProtocolVersion = 1;

enum class TransferStatus : std::uint32_t {
    kSuccess = 0,
    kInvalidControl = 1,
    kLengthMismatch = 2,
    kChecksumMismatch = 3,
    kInvalidBundle = 4,
    kWrongTarget = 5,
};

struct ReplicaRequestWire {
    std::uint32_t magic_be{};
    std::uint32_t version_be{};
    std::uint32_t bundle_bytes_be{};
    std::uint32_t bucket_id_be{};
    std::uint32_t target_node_be{};
    std::uint32_t checksum_be{};
};

struct MemoryDescriptor {
    std::uint64_t address_be{};
    std::uint32_t rkey_be{};
    std::uint32_t length_be{};
};

struct ControlMessage {
    std::uint32_t magic_be{};
    std::uint32_t length_be{};
    std::uint32_t status_be{};
    std::uint32_t bucket_id_be{};
    std::uint32_t checksum_be{};
};

static_assert(sizeof(ReplicaRequestWire) == 24, "unexpected replica request layout");
static_assert(sizeof(MemoryDescriptor) == 16, "unexpected memory descriptor layout");
static_assert(sizeof(ControlMessage) == 20, "unexpected control-message layout");

struct EventChannel {
    rdma_event_channel* value{};

    EventChannel() : value(rdma_create_event_channel()) {
        if (value == nullptr) {
            throw std::runtime_error("rdma_create_event_channel failed");
        }
    }

    ~EventChannel() {
        if (value != nullptr) {
            rdma_destroy_event_channel(value);
        }
    }

    EventChannel(const EventChannel&) = delete;
    EventChannel& operator=(const EventChannel&) = delete;
};

struct Listener {
    rdma_cm_id* id{};

    ~Listener() {
        if (id != nullptr) {
            rdma_destroy_id(id);
        }
    }

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener() = default;
};

struct ConnectionResources {
    rdma_cm_id* id{};
    ibv_pd* pd{};
    ibv_cq* send_cq{};
    ibv_cq* receive_cq{};
    ibv_mr* data_mr{};
    ibv_mr* send_control_mr{};
    ibv_mr* receive_control_mr{};
    std::vector<std::uint8_t> data;
    ControlMessage send_control{};
    ControlMessage receive_control{};

    ~ConnectionResources() {
        if (id != nullptr && id->qp != nullptr) {
            rdma_destroy_qp(id);
        }
        if (receive_control_mr != nullptr) {
            ibv_dereg_mr(receive_control_mr);
        }
        if (send_control_mr != nullptr) {
            ibv_dereg_mr(send_control_mr);
        }
        if (data_mr != nullptr) {
            ibv_dereg_mr(data_mr);
        }
        if (receive_cq != nullptr) {
            ibv_destroy_cq(receive_cq);
        }
        if (send_cq != nullptr) {
            ibv_destroy_cq(send_cq);
        }
        if (pd != nullptr) {
            ibv_dealloc_pd(pd);
        }
        if (id != nullptr) {
            rdma_destroy_id(id);
        }
    }

    ConnectionResources(const ConnectionResources&) = delete;
    ConnectionResources& operator=(const ConnectionResources&) = delete;
    ConnectionResources() = default;

    std::vector<std::uint8_t> ReleaseRegisteredData() {
        if (data_mr == nullptr) {
            throw std::logic_error("RDMA data region has already been released");
        }
        const int result = ibv_dereg_mr(data_mr);
        if (result != 0) {
            const int error_code = result > 0 ? result : errno;
            throw std::runtime_error(
                "ibv_dereg_mr(committed data): " +
                std::string(std::strerror(error_code)));
        }
        data_mr = nullptr;
        return std::move(data);
    }
};

struct StoredReplica {
    ReplicaBundle bundle;
    std::vector<std::uint8_t> encoded;
    std::size_t generation{};
};

void Check(int result, const std::string& operation) {
    if (result != 0) {
        const int error_code = result > 0 ? result : errno;
        throw std::runtime_error(operation + ": " + std::strerror(error_code));
    }
}

rdma_cm_event* WaitEvent(rdma_event_channel* channel,
                         rdma_cm_event_type expected,
                         int timeout_ms = kCmEventTimeoutMs) {
    pollfd descriptor{};
    descriptor.fd = channel->fd;
    descriptor.events = POLLIN;
    int poll_result = 0;
    do {
        poll_result = poll(&descriptor, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
        throw std::runtime_error(
            "timed out waiting for CM event " + std::string(rdma_event_str(expected)));
    }
    if (poll_result < 0) {
        throw std::runtime_error(
            "poll(RDMA CM event channel): " + std::string(std::strerror(errno)));
    }
    if ((descriptor.revents & POLLIN) == 0) {
        throw std::runtime_error("RDMA CM event channel became unavailable");
    }

    rdma_cm_event* event = nullptr;
    Check(rdma_get_cm_event(channel, &event), "rdma_get_cm_event");
    if (event->event != expected) {
        const std::string actual = rdma_event_str(event->event);
        const int status = event->status;
        Check(rdma_ack_cm_event(event), "rdma_ack_cm_event(unexpected)");
        throw std::runtime_error(
            "expected CM event " + std::string(rdma_event_str(expected)) +
            ", received " + actual + ", status=" + std::to_string(status));
    }
    return event;
}

void ThrowIfConnectionClosed(rdma_event_channel* channel) {
    pollfd descriptor{};
    descriptor.fd = channel->fd;
    descriptor.events = POLLIN;
    int poll_result = 0;
    do {
        poll_result = poll(&descriptor, 1, 0);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result < 0) {
        throw std::runtime_error(
            "poll(RDMA CM event channel): " + std::string(std::strerror(errno)));
    }
    if (poll_result == 0) {
        return;
    }
    if ((descriptor.revents & POLLIN) == 0) {
        throw std::runtime_error("RDMA CM event channel became unavailable");
    }

    rdma_cm_event* event = nullptr;
    Check(rdma_get_cm_event(channel, &event), "rdma_get_cm_event");
    const auto event_type = event->event;
    const int status = event->status;
    Check(rdma_ack_cm_event(event), "rdma_ack_cm_event(connection event)");
    if (event_type == RDMA_CM_EVENT_DISCONNECTED) {
        throw std::runtime_error("peer disconnected before replication completed");
    }
    throw std::runtime_error(
        "unexpected CM event while waiting for completion: " +
        std::string(rdma_event_str(event_type)) + ", status=" + std::to_string(status));
}

sockaddr_in Address(const std::string& ip, std::uint16_t port, bool passive) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = passive ? INADDR_ANY : 0;
    if (!passive && inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("invalid IPv4 address: " + ip);
    }
    return address;
}

void CreateQueuePair(ConnectionResources& resources,
                     std::size_t bytes,
                     bool remote_target) {
    if (bytes == 0 || bytes > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("RDMA bundle length is outside the protocol limit");
    }
    resources.pd = ibv_alloc_pd(resources.id->verbs);
    if (resources.pd == nullptr) {
        throw std::runtime_error("ibv_alloc_pd failed");
    }
    resources.send_cq = ibv_create_cq(resources.id->verbs, 8, nullptr, nullptr, 0);
    resources.receive_cq = ibv_create_cq(resources.id->verbs, 8, nullptr, nullptr, 0);
    if (resources.send_cq == nullptr || resources.receive_cq == nullptr) {
        throw std::runtime_error("ibv_create_cq failed");
    }

    ibv_qp_init_attr attributes{};
    attributes.qp_type = IBV_QPT_RC;
    attributes.send_cq = resources.send_cq;
    attributes.recv_cq = resources.receive_cq;
    attributes.cap.max_send_wr = 4;
    attributes.cap.max_recv_wr = 4;
    attributes.cap.max_send_sge = 1;
    attributes.cap.max_recv_sge = 1;
    Check(rdma_create_qp(resources.id, resources.pd, &attributes), "rdma_create_qp");

    if (resources.data.empty()) {
        resources.data.resize(bytes, 0);
    } else if (resources.data.size() != bytes) {
        throw std::logic_error("preloaded RDMA data does not match the requested region size");
    }
    int data_access = IBV_ACCESS_LOCAL_WRITE;
    if (remote_target) {
        data_access |= IBV_ACCESS_REMOTE_WRITE;
    }
    resources.data_mr = ibv_reg_mr(
        resources.pd, resources.data.data(), resources.data.size(), data_access);
    if (resources.data_mr == nullptr) {
        throw std::runtime_error("ibv_reg_mr(replica bundle) failed");
    }
    resources.send_control_mr = ibv_reg_mr(
        resources.pd,
        &resources.send_control,
        sizeof(resources.send_control),
        IBV_ACCESS_LOCAL_WRITE);
    resources.receive_control_mr = ibv_reg_mr(
        resources.pd,
        &resources.receive_control,
        sizeof(resources.receive_control),
        IBV_ACCESS_LOCAL_WRITE);
    if (resources.send_control_mr == nullptr || resources.receive_control_mr == nullptr) {
        throw std::runtime_error("ibv_reg_mr(replication control) failed");
    }
}

void PostReceive(ConnectionResources& resources) {
    ibv_sge scatter{};
    scatter.addr = reinterpret_cast<std::uintptr_t>(&resources.receive_control);
    scatter.length = sizeof(resources.receive_control);
    scatter.lkey = resources.receive_control_mr->lkey;
    ibv_recv_wr request{};
    request.wr_id = 1;
    request.sg_list = &scatter;
    request.num_sge = 1;
    ibv_recv_wr* bad_request = nullptr;
    Check(ibv_post_recv(resources.id->qp, &request, &bad_request), "ibv_post_recv");
}

void PostControl(ConnectionResources& resources, std::uint64_t work_request_id) {
    ibv_sge scatter{};
    scatter.addr = reinterpret_cast<std::uintptr_t>(&resources.send_control);
    scatter.length = sizeof(resources.send_control);
    scatter.lkey = resources.send_control_mr->lkey;
    ibv_send_wr request{};
    request.wr_id = work_request_id;
    request.sg_list = &scatter;
    request.num_sge = 1;
    request.opcode = IBV_WR_SEND;
    request.send_flags = IBV_SEND_SIGNALED;
    ibv_send_wr* bad_request = nullptr;
    Check(ibv_post_send(resources.id->qp, &request, &bad_request),
          "ibv_post_send(control)");
}

ibv_wc WaitCompletion(rdma_event_channel* channel,
                      ibv_cq* cq,
                      std::uint64_t expected_work_request_id,
                      ibv_wc_opcode expected_opcode) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kCompletionTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        ibv_wc completion{};
        const int count = ibv_poll_cq(cq, 1, &completion);
        if (count < 0) {
            throw std::runtime_error("ibv_poll_cq failed");
        }
        if (count == 1) {
            if (completion.status != IBV_WC_SUCCESS) {
                throw std::runtime_error(
                    "work completion failed: " +
                    std::string(ibv_wc_status_str(completion.status)));
            }
            if (completion.wr_id != expected_work_request_id ||
                completion.opcode != expected_opcode) {
                throw std::runtime_error(
                    "unexpected work completion: wr_id=" +
                    std::to_string(completion.wr_id) + ", opcode=" +
                    std::to_string(completion.opcode));
            }
            return completion;
        }
        ThrowIfConnectionClosed(channel);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    throw std::runtime_error("replication completion queue timeout");
}

ReplicaRequestWire ParseRequest(const rdma_cm_event& event) {
    if (event.param.conn.private_data == nullptr ||
        event.param.conn.private_data_len < sizeof(ReplicaRequestWire)) {
        throw std::invalid_argument("client did not provide a replica request");
    }
    ReplicaRequestWire request{};
    std::memcpy(&request, event.param.conn.private_data, sizeof(request));
    if (ntohl(request.magic_be) != kRequestMagic ||
        ntohl(request.version_be) != kProtocolVersion) {
        throw std::invalid_argument("client used an unsupported replication protocol");
    }
    return request;
}

std::string StatusName(TransferStatus status) {
    switch (status) {
        case TransferStatus::kSuccess:
            return "success";
        case TransferStatus::kInvalidControl:
            return "invalid-control";
        case TransferStatus::kLengthMismatch:
            return "length-mismatch";
        case TransferStatus::kChecksumMismatch:
            return "checksum-mismatch";
        case TransferStatus::kInvalidBundle:
            return "invalid-bundle";
        case TransferStatus::kWrongTarget:
            return "wrong-target";
    }
    return "unknown";
}

}  // namespace

RdmaReplicationResult ReplicateBundleRdma(
    const std::string& server_ip,
    std::uint16_t port,
    const std::vector<std::uint8_t>& encoded_bundle) {
    const auto overall_started = std::chrono::steady_clock::now();
    const ReplicaBundle bundle = DecodeReplicaBundle(encoded_bundle);
    if (encoded_bundle.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("encoded replica bundle exceeds the RDMA protocol limit");
    }
    if (bundle.bucket_id > std::numeric_limits<std::uint32_t>::max() ||
        bundle.target_node > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("replica metadata exceeds the RDMA protocol limit");
    }
    const std::uint32_t checksum = ReplicaBundleChecksum(encoded_bundle);

    EventChannel channel;
    ConnectionResources resources;
    Check(rdma_create_id(channel.value, &resources.id, nullptr, RDMA_PS_TCP),
          "rdma_create_id(client)");
    auto address = Address(server_ip, port, false);
    Check(rdma_resolve_addr(
              resources.id,
              nullptr,
              reinterpret_cast<sockaddr*>(&address),
              kResolveTimeoutMs),
          "rdma_resolve_addr");
    rdma_cm_event* resolved = WaitEvent(channel.value, RDMA_CM_EVENT_ADDR_RESOLVED);
    Check(rdma_ack_cm_event(resolved), "rdma_ack_cm_event(address resolved)");
    Check(rdma_resolve_route(resources.id, kResolveTimeoutMs), "rdma_resolve_route");
    rdma_cm_event* routed = WaitEvent(channel.value, RDMA_CM_EVENT_ROUTE_RESOLVED);
    Check(rdma_ack_cm_event(routed), "rdma_ack_cm_event(route resolved)");

    resources.data = encoded_bundle;
    CreateQueuePair(resources, encoded_bundle.size(), false);
    PostReceive(resources);

    ReplicaRequestWire request{};
    request.magic_be = htonl(kRequestMagic);
    request.version_be = htonl(kProtocolVersion);
    request.bundle_bytes_be = htonl(static_cast<std::uint32_t>(encoded_bundle.size()));
    request.bucket_id_be = htonl(static_cast<std::uint32_t>(bundle.bucket_id));
    request.target_node_be = htonl(static_cast<std::uint32_t>(bundle.target_node));
    request.checksum_be = htonl(checksum);
    rdma_conn_param connection{};
    connection.private_data = &request;
    connection.private_data_len = sizeof(request);
    connection.responder_resources = 1;
    connection.initiator_depth = 1;
    connection.retry_count = 7;
    Check(rdma_connect(resources.id, &connection), "rdma_connect");
    rdma_cm_event* established = WaitEvent(channel.value, RDMA_CM_EVENT_ESTABLISHED);
    if (established->param.conn.private_data == nullptr ||
        established->param.conn.private_data_len < sizeof(MemoryDescriptor)) {
        Check(rdma_ack_cm_event(established), "rdma_ack_cm_event(invalid established)");
        throw std::runtime_error("replica server did not provide a memory descriptor");
    }
    MemoryDescriptor descriptor{};
    std::memcpy(&descriptor, established->param.conn.private_data, sizeof(descriptor));
    Check(rdma_ack_cm_event(established), "rdma_ack_cm_event(client established)");
    if (ntohl(descriptor.length_be) != encoded_bundle.size()) {
        Check(rdma_disconnect(resources.id), "rdma_disconnect(length mismatch)");
        throw std::runtime_error("replica server registered an unexpected region length");
    }

    ibv_sge scatter{};
    scatter.addr = reinterpret_cast<std::uintptr_t>(resources.data.data());
    scatter.length = static_cast<std::uint32_t>(resources.data.size());
    scatter.lkey = resources.data_mr->lkey;
    ibv_send_wr write_request{};
    write_request.wr_id = 2;
    write_request.sg_list = &scatter;
    write_request.num_sge = 1;
    write_request.opcode = IBV_WR_RDMA_WRITE;
    write_request.send_flags = IBV_SEND_SIGNALED;
    write_request.wr.rdma.remote_addr = be64toh(descriptor.address_be);
    write_request.wr.rdma.rkey = ntohl(descriptor.rkey_be);
    ibv_send_wr* bad_request = nullptr;
    const auto write_started = std::chrono::steady_clock::now();
    Check(ibv_post_send(resources.id->qp, &write_request, &bad_request),
          "ibv_post_send(RDMA_WRITE replica)");
    WaitCompletion(channel.value, resources.send_cq, 2, IBV_WC_RDMA_WRITE);
    const auto write_finished = std::chrono::steady_clock::now();

    resources.send_control.magic_be = htonl(kDoneMagic);
    resources.send_control.length_be =
        htonl(static_cast<std::uint32_t>(encoded_bundle.size()));
    resources.send_control.status_be = htonl(0);
    resources.send_control.bucket_id_be =
        htonl(static_cast<std::uint32_t>(bundle.bucket_id));
    resources.send_control.checksum_be = htonl(checksum);
    PostControl(resources, 3);
    WaitCompletion(channel.value, resources.send_cq, 3, IBV_WC_SEND);

    const ibv_wc acknowledgement =
        WaitCompletion(channel.value, resources.receive_cq, 1, IBV_WC_RECV);
    const auto status = static_cast<TransferStatus>(
        ntohl(resources.receive_control.status_be));
    if (acknowledgement.byte_len != sizeof(ControlMessage) ||
        ntohl(resources.receive_control.magic_be) != kAckMagic ||
        ntohl(resources.receive_control.length_be) != encoded_bundle.size() ||
        ntohl(resources.receive_control.bucket_id_be) != bundle.bucket_id ||
        ntohl(resources.receive_control.checksum_be) != checksum ||
        status != TransferStatus::kSuccess) {
        rdma_disconnect(resources.id);
        throw std::runtime_error(
            "replica server rejected the bucket bundle; status=" + StatusName(status));
    }

    const std::string provider = ibv_get_device_name(resources.id->verbs->device);
    Check(rdma_disconnect(resources.id), "rdma_disconnect");
    rdma_cm_event* disconnected = WaitEvent(channel.value, RDMA_CM_EVENT_DISCONNECTED);
    Check(rdma_ack_cm_event(disconnected), "rdma_ack_cm_event(client disconnected)");
    const auto overall_finished = std::chrono::steady_clock::now();
    return RdmaReplicationResult{
        bundle.bucket_id,
        bundle.target_node,
        encoded_bundle.size(),
        std::chrono::duration<double, std::milli>(write_finished - write_started).count(),
        std::chrono::duration<double, std::milli>(overall_finished - overall_started).count(),
        provider,
    };
}

void RunRdmaReplicaServer(std::uint16_t port,
                          std::size_t node_id,
                          std::size_t maximum_replications,
                          std::size_t maximum_bundle_bytes) {
    if (node_id > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("node_id exceeds the replication protocol limit");
    }
    if (maximum_bundle_bytes == 0 ||
        maximum_bundle_bytes > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("maximum_bundle_bytes is outside the protocol limit");
    }

    EventChannel channel;
    Listener listener;
    Check(rdma_create_id(channel.value, &listener.id, nullptr, RDMA_PS_TCP),
          "rdma_create_id(listener)");
    auto address = Address("", port, true);
    Check(rdma_bind_addr(listener.id, reinterpret_cast<sockaddr*>(&address)),
          "rdma_bind_addr");
    Check(rdma_listen(listener.id, 8), "rdma_listen");
    std::cout << "replica_server_ready node=" << node_id << " port=" << port
              << " max_bundle_bytes=" << maximum_bundle_bytes << std::endl;

    std::map<std::size_t, StoredReplica> replicas;
    std::size_t attempts = 0;
    std::size_t generation = 0;
    while (maximum_replications == 0 || attempts < maximum_replications) {
        rdma_cm_event* request_event =
            WaitEvent(
                channel.value, RDMA_CM_EVENT_CONNECT_REQUEST, kServerIdleTimeoutMs);
        ConnectionResources resources;
        resources.id = request_event->id;
        ReplicaRequestWire request{};
        try {
            request = ParseRequest(*request_event);
        } catch (...) {
            Check(rdma_ack_cm_event(request_event),
                  "rdma_ack_cm_event(invalid connect request)");
            throw;
        }
        Check(rdma_ack_cm_event(request_event), "rdma_ack_cm_event(connect request)");
        ++attempts;

        const std::size_t bytes = ntohl(request.bundle_bytes_be);
        const std::size_t requested_bucket = ntohl(request.bucket_id_be);
        const std::size_t requested_target = ntohl(request.target_node_be);
        const std::uint32_t requested_checksum = ntohl(request.checksum_be);
        if (bytes == 0 || bytes > maximum_bundle_bytes) {
            Check(rdma_reject(resources.id, nullptr, 0), "rdma_reject(bundle length)");
            throw std::invalid_argument("client requested an invalid replica bundle length");
        }
        if (requested_target != node_id) {
            Check(rdma_reject(resources.id, nullptr, 0), "rdma_reject(target node)");
            throw std::invalid_argument("client connected to the wrong target memory node");
        }

        CreateQueuePair(resources, bytes, true);
        PostReceive(resources);
        MemoryDescriptor descriptor{};
        descriptor.address_be = htobe64(static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(resources.data.data())));
        descriptor.rkey_be = htonl(resources.data_mr->rkey);
        descriptor.length_be = htonl(static_cast<std::uint32_t>(resources.data.size()));
        rdma_conn_param connection{};
        connection.private_data = &descriptor;
        connection.private_data_len = sizeof(descriptor);
        connection.responder_resources = 1;
        connection.initiator_depth = 1;
        connection.retry_count = 7;
        Check(rdma_accept(resources.id, &connection), "rdma_accept");
        rdma_cm_event* established = WaitEvent(channel.value, RDMA_CM_EVENT_ESTABLISHED);
        Check(rdma_ack_cm_event(established), "rdma_ack_cm_event(server established)");

        const ibv_wc completion =
            WaitCompletion(channel.value, resources.receive_cq, 1, IBV_WC_RECV);
        TransferStatus status = TransferStatus::kSuccess;
        std::string failure_reason;
        ReplicaBundle decoded;
        const std::size_t reported_bytes = ntohl(resources.receive_control.length_be);
        if (completion.byte_len != sizeof(ControlMessage) ||
            ntohl(resources.receive_control.magic_be) != kDoneMagic ||
            ntohl(resources.receive_control.status_be) != 0 ||
            ntohl(resources.receive_control.bucket_id_be) != requested_bucket) {
            status = TransferStatus::kInvalidControl;
            failure_reason = "invalid completion control message";
        } else if (reported_bytes != bytes) {
            status = TransferStatus::kLengthMismatch;
            failure_reason = "client reported a different bundle length";
        } else if (ntohl(resources.receive_control.checksum_be) != requested_checksum ||
                   ReplicaBundleChecksum(resources.data) != requested_checksum) {
            status = TransferStatus::kChecksumMismatch;
            failure_reason = "received replica bytes failed checksum validation";
        } else {
            try {
                decoded = DecodeReplicaBundle(resources.data);
                if (decoded.bucket_id != requested_bucket) {
                    status = TransferStatus::kInvalidBundle;
                    failure_reason = "bundle bucket does not match its request";
                } else if (decoded.target_node != node_id) {
                    status = TransferStatus::kWrongTarget;
                    failure_reason = "bundle target does not match this memory node";
                }
            } catch (const std::exception& error) {
                status = TransferStatus::kInvalidBundle;
                failure_reason = error.what();
            }
        }

        if (status == TransferStatus::kSuccess) {
            auto committed_bytes = resources.ReleaseRegisteredData();
            replicas[decoded.bucket_id] = StoredReplica{
                decoded,
                std::move(committed_bytes),
                ++generation,
            };
        }

        resources.send_control.magic_be = htonl(kAckMagic);
        resources.send_control.length_be = htonl(static_cast<std::uint32_t>(bytes));
        resources.send_control.status_be = htonl(static_cast<std::uint32_t>(status));
        resources.send_control.bucket_id_be =
            htonl(static_cast<std::uint32_t>(requested_bucket));
        resources.send_control.checksum_be = htonl(requested_checksum);
        PostControl(resources, 4);
        WaitCompletion(channel.value, resources.send_cq, 4, IBV_WC_SEND);
        rdma_cm_event* disconnected = WaitEvent(channel.value, RDMA_CM_EVENT_DISCONNECTED);
        Check(rdma_ack_cm_event(disconnected), "rdma_ack_cm_event(disconnected)");

        if (status == TransferStatus::kSuccess) {
            const auto& stored = replicas.at(requested_bucket);
            std::cout << "replica_committed bucket=" << requested_bucket
                      << " target_node=" << node_id
                      << " records=" << stored.bundle.records.size()
                      << " bytes=" << stored.encoded.size()
                      << " expires_after_window=" << stored.bundle.expires_after_window
                      << " generation=" << stored.generation << std::endl;
        } else {
            std::cerr << "replica_rejected bucket=" << requested_bucket
                      << " target_node=" << node_id
                      << " status=" << StatusName(status)
                      << " reason=" << failure_reason << std::endl;
        }
    }

    std::cout << "replica_server_complete node=" << node_id
              << " attempts=" << attempts
              << " stored_buckets=" << replicas.size() << std::endl;
}

}  // namespace hotbucket
