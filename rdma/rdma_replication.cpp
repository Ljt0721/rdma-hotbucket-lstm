#include "hotbucket/rdma_replication.hpp"

#include "hotbucket/replica_bundle.hpp"

#include <arpa/inet.h>
#include <endian.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <poll.h>
#include <rdma/rdma_cma.h>

#include <algorithm>
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
constexpr std::uint32_t kSessionMagic = 0x48425351U;
constexpr std::uint32_t kDescriptorMagic = 0x4842534DU;
constexpr std::uint32_t kDoneMagic = 0x48425244U;
constexpr std::uint32_t kAckMagic = 0x48425241U;
constexpr std::uint32_t kProtocolVersion = 2;

enum class TransferStatus : std::uint32_t {
    kSuccess = 0,
    kInvalidControl = 1,
    kLengthMismatch = 2,
    kChecksumMismatch = 3,
    kInvalidBundle = 4,
    kWrongTarget = 5,
    kInvalidSequence = 6,
};

struct SessionRequestWire {
    std::uint32_t magic_be{};
    std::uint32_t version_be{};
    std::uint32_t staging_bytes_be{};
    std::uint32_t target_node_be{};
};

struct MemoryDescriptor {
    std::uint32_t magic_be{};
    std::uint32_t version_be{};
    std::uint64_t address_be{};
    std::uint32_t rkey_be{};
    std::uint32_t length_be{};
    std::uint32_t target_node_be{};
    std::uint32_t reserved_be{};
};

struct ControlMessage {
    std::uint32_t magic_be{};
    std::uint32_t version_be{};
    std::uint64_t sequence_be{};
    std::uint32_t length_be{};
    std::uint32_t bucket_id_be{};
    std::uint32_t target_node_be{};
    std::uint32_t checksum_be{};
    std::uint32_t status_be{};
    std::uint32_t reserved_be{};
};

static_assert(sizeof(SessionRequestWire) == 16, "unexpected session request layout");
static_assert(sizeof(MemoryDescriptor) == 32, "unexpected memory descriptor layout");
static_assert(sizeof(ControlMessage) == 40, "unexpected control-message layout");

class PeerDisconnected final : public std::runtime_error {
public:
    PeerDisconnected() : std::runtime_error("RDMA peer disconnected") {}
};

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
};

struct StoredReplica {
    ReplicaBundle bundle;
    std::vector<std::uint8_t> encoded;
    std::size_t generation{};
    std::uint64_t sequence{};
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
        throw PeerDisconnected();
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
        throw std::invalid_argument("RDMA staging length is outside the protocol limit");
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

    resources.data.assign(bytes, 0);
    int data_access = IBV_ACCESS_LOCAL_WRITE;
    if (remote_target) {
        data_access |= IBV_ACCESS_REMOTE_WRITE;
    }
    resources.data_mr = ibv_reg_mr(
        resources.pd, resources.data.data(), resources.data.size(), data_access);
    if (resources.data_mr == nullptr) {
        throw std::runtime_error("ibv_reg_mr(replica staging buffer) failed");
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
    resources.receive_control = ControlMessage{};
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

SessionRequestWire ParseSessionRequest(const rdma_cm_event& event) {
    if (event.param.conn.private_data == nullptr ||
        event.param.conn.private_data_len < sizeof(SessionRequestWire)) {
        throw std::invalid_argument("client did not provide an RDMA session request");
    }
    SessionRequestWire request{};
    std::memcpy(&request, event.param.conn.private_data, sizeof(request));
    if (ntohl(request.magic_be) != kSessionMagic ||
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
        case TransferStatus::kInvalidSequence:
            return "invalid-sequence";
    }
    return "unknown";
}

}  // namespace

struct RdmaReplicaSession::Impl {
    EventChannel channel;
    ConnectionResources resources;
    std::size_t target_node{};
    std::size_t staging_bytes{};
    std::uint64_t remote_address{};
    std::uint32_t remote_rkey{};
    std::uint64_t next_sequence{1};
    double setup_ms{};
    std::string provider;
    bool connected{false};

    Impl(const std::string& server_ip,
         std::uint16_t port,
         std::size_t requested_target_node,
         std::size_t requested_staging_bytes)
        : target_node(requested_target_node), staging_bytes(requested_staging_bytes) {
        if (target_node > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("target node exceeds the RDMA protocol limit");
        }
        if (staging_bytes == 0 ||
            staging_bytes > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("staging bytes are outside the RDMA protocol limit");
        }

        const auto setup_started = std::chrono::steady_clock::now();
        Check(rdma_create_id(channel.value, &resources.id, nullptr, RDMA_PS_TCP),
              "rdma_create_id(client)");
        auto address = Address(server_ip, port, false);
        Check(rdma_resolve_addr(
                  resources.id,
                  nullptr,
                  reinterpret_cast<sockaddr*>(&address),
                  kResolveTimeoutMs),
              "rdma_resolve_addr");
        rdma_cm_event* resolved =
            WaitEvent(channel.value, RDMA_CM_EVENT_ADDR_RESOLVED);
        Check(rdma_ack_cm_event(resolved), "rdma_ack_cm_event(address resolved)");
        Check(rdma_resolve_route(resources.id, kResolveTimeoutMs), "rdma_resolve_route");
        rdma_cm_event* routed =
            WaitEvent(channel.value, RDMA_CM_EVENT_ROUTE_RESOLVED);
        Check(rdma_ack_cm_event(routed), "rdma_ack_cm_event(route resolved)");

        CreateQueuePair(resources, staging_bytes, false);
        SessionRequestWire request{};
        request.magic_be = htonl(kSessionMagic);
        request.version_be = htonl(kProtocolVersion);
        request.staging_bytes_be = htonl(static_cast<std::uint32_t>(staging_bytes));
        request.target_node_be = htonl(static_cast<std::uint32_t>(target_node));
        rdma_conn_param connection{};
        connection.private_data = &request;
        connection.private_data_len = sizeof(request);
        connection.responder_resources = 1;
        connection.initiator_depth = 1;
        connection.retry_count = 7;
        Check(rdma_connect(resources.id, &connection), "rdma_connect");
        rdma_cm_event* established =
            WaitEvent(channel.value, RDMA_CM_EVENT_ESTABLISHED);
        if (established->param.conn.private_data == nullptr ||
            established->param.conn.private_data_len < sizeof(MemoryDescriptor)) {
            Check(rdma_ack_cm_event(established),
                  "rdma_ack_cm_event(invalid established)");
            throw std::runtime_error("replica server did not provide a memory descriptor");
        }
        MemoryDescriptor descriptor{};
        std::memcpy(&descriptor, established->param.conn.private_data, sizeof(descriptor));
        Check(rdma_ack_cm_event(established), "rdma_ack_cm_event(client established)");
        if (ntohl(descriptor.magic_be) != kDescriptorMagic ||
            ntohl(descriptor.version_be) != kProtocolVersion ||
            ntohl(descriptor.length_be) != staging_bytes ||
            ntohl(descriptor.target_node_be) != target_node) {
            rdma_disconnect(resources.id);
            throw std::runtime_error("replica server returned an invalid memory descriptor");
        }
        remote_address = be64toh(descriptor.address_be);
        remote_rkey = ntohl(descriptor.rkey_be);
        provider = ibv_get_device_name(resources.id->verbs->device);
        connected = true;
        setup_ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - setup_started)
                       .count();
    }

    ~Impl() {
        DisconnectNoThrow();
    }

    void DisconnectNoThrow() noexcept {
        if (!connected || resources.id == nullptr) {
            return;
        }
        if (rdma_disconnect(resources.id) == 0) {
            try {
                rdma_cm_event* disconnected =
                    WaitEvent(channel.value, RDMA_CM_EVENT_DISCONNECTED, 2000);
                rdma_ack_cm_event(disconnected);
            } catch (...) {
            }
        }
        connected = false;
    }

    RdmaReplicationResult Replicate(
        const std::vector<std::uint8_t>& encoded_bundle) {
        if (!connected) {
            throw std::runtime_error("RDMA replica session is not connected");
        }
        const auto operation_started = std::chrono::steady_clock::now();
        const ReplicaBundle bundle = DecodeReplicaBundle(encoded_bundle);
        if (bundle.target_node != target_node) {
            throw std::invalid_argument(
                "replica bundle target does not match the persistent session");
        }
        if (encoded_bundle.size() > staging_bytes) {
            throw std::length_error("replica bundle exceeds the persistent staging buffer");
        }
        if (next_sequence == 0) {
            throw std::overflow_error("RDMA replication sequence number wrapped");
        }
        const std::uint64_t sequence = next_sequence++;
        const std::uint32_t checksum = ReplicaBundleChecksum(encoded_bundle);
        std::copy(encoded_bundle.begin(), encoded_bundle.end(), resources.data.begin());
        PostReceive(resources);

        ibv_sge scatter{};
        scatter.addr = reinterpret_cast<std::uintptr_t>(resources.data.data());
        scatter.length = static_cast<std::uint32_t>(encoded_bundle.size());
        scatter.lkey = resources.data_mr->lkey;
        ibv_send_wr write_request{};
        write_request.wr_id = 2;
        write_request.sg_list = &scatter;
        write_request.num_sge = 1;
        write_request.opcode = IBV_WR_RDMA_WRITE;
        write_request.send_flags = IBV_SEND_SIGNALED;
        write_request.wr.rdma.remote_addr = remote_address;
        write_request.wr.rdma.rkey = remote_rkey;
        ibv_send_wr* bad_request = nullptr;
        const auto write_started = std::chrono::steady_clock::now();
        Check(ibv_post_send(resources.id->qp, &write_request, &bad_request),
              "ibv_post_send(RDMA_WRITE replica)");
        WaitCompletion(channel.value, resources.send_cq, 2, IBV_WC_RDMA_WRITE);
        const auto write_finished = std::chrono::steady_clock::now();

        resources.send_control = ControlMessage{};
        resources.send_control.magic_be = htonl(kDoneMagic);
        resources.send_control.version_be = htonl(kProtocolVersion);
        resources.send_control.sequence_be = htobe64(sequence);
        resources.send_control.length_be =
            htonl(static_cast<std::uint32_t>(encoded_bundle.size()));
        resources.send_control.bucket_id_be =
            htonl(static_cast<std::uint32_t>(bundle.bucket_id));
        resources.send_control.target_node_be =
            htonl(static_cast<std::uint32_t>(bundle.target_node));
        resources.send_control.checksum_be = htonl(checksum);
        PostControl(resources, 3);
        WaitCompletion(channel.value, resources.send_cq, 3, IBV_WC_SEND);

        const ibv_wc acknowledgement =
            WaitCompletion(channel.value, resources.receive_cq, 1, IBV_WC_RECV);
        const auto status = static_cast<TransferStatus>(
            ntohl(resources.receive_control.status_be));
        if (acknowledgement.byte_len != sizeof(ControlMessage) ||
            ntohl(resources.receive_control.magic_be) != kAckMagic ||
            ntohl(resources.receive_control.version_be) != kProtocolVersion ||
            be64toh(resources.receive_control.sequence_be) != sequence ||
            ntohl(resources.receive_control.length_be) != encoded_bundle.size() ||
            ntohl(resources.receive_control.bucket_id_be) != bundle.bucket_id ||
            ntohl(resources.receive_control.target_node_be) != bundle.target_node ||
            ntohl(resources.receive_control.checksum_be) != checksum ||
            status != TransferStatus::kSuccess) {
            throw std::runtime_error(
                "replica server rejected sequence " + std::to_string(sequence) +
                "; status=" + StatusName(status));
        }

        const auto operation_finished = std::chrono::steady_clock::now();
        return RdmaReplicationResult{
            bundle.bucket_id,
            bundle.target_node,
            encoded_bundle.size(),
            sequence,
            std::chrono::duration<double, std::milli>(
                write_finished - write_started)
                .count(),
            std::chrono::duration<double, std::milli>(
                operation_finished - operation_started)
                .count(),
            provider,
        };
    }
};

RdmaReplicaSession::RdmaReplicaSession(const std::string& server_ip,
                                       std::uint16_t port,
                                       std::size_t target_node,
                                       std::size_t staging_bytes)
    : impl_(std::make_unique<Impl>(
          server_ip, port, target_node, staging_bytes)) {}

RdmaReplicaSession::~RdmaReplicaSession() = default;
RdmaReplicaSession::RdmaReplicaSession(RdmaReplicaSession&&) noexcept = default;
RdmaReplicaSession& RdmaReplicaSession::operator=(RdmaReplicaSession&&) noexcept = default;

RdmaReplicationResult RdmaReplicaSession::Replicate(
    const std::vector<std::uint8_t>& encoded_bundle) {
    if (!impl_) {
        throw std::logic_error("RDMA replica session was moved from");
    }
    return impl_->Replicate(encoded_bundle);
}

std::size_t RdmaReplicaSession::target_node() const noexcept {
    return impl_ ? impl_->target_node : 0;
}

std::size_t RdmaReplicaSession::staging_bytes() const noexcept {
    return impl_ ? impl_->staging_bytes : 0;
}

double RdmaReplicaSession::setup_ms() const noexcept {
    return impl_ ? impl_->setup_ms : 0.0;
}

RdmaReplicationResult ReplicateBundleRdma(
    const std::string& server_ip,
    std::uint16_t port,
    const std::vector<std::uint8_t>& encoded_bundle) {
    const auto bundle = DecodeReplicaBundle(encoded_bundle);
    const std::size_t staging_bytes =
        std::max(kDefaultRdmaStagingBytes, encoded_bundle.size());
    RdmaReplicaSession session(
        server_ip, port, bundle.target_node, staging_bytes);
    return session.Replicate(encoded_bundle);
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
    std::size_t session_count = 0;
    while (maximum_replications == 0 || attempts < maximum_replications) {
        const int idle_timeout = maximum_replications == 0 ? -1 : kServerIdleTimeoutMs;
        rdma_cm_event* request_event = WaitEvent(
            channel.value, RDMA_CM_EVENT_CONNECT_REQUEST, idle_timeout);
        ConnectionResources resources;
        resources.id = request_event->id;
        SessionRequestWire request{};
        try {
            request = ParseSessionRequest(*request_event);
        } catch (const std::exception& error) {
            Check(rdma_ack_cm_event(request_event),
                  "rdma_ack_cm_event(invalid session request)");
            rdma_reject(resources.id, nullptr, 0);
            std::cerr << "replica_session_rejected node=" << node_id
                      << " reason=" << error.what() << std::endl;
            continue;
        }
        Check(rdma_ack_cm_event(request_event), "rdma_ack_cm_event(connect request)");

        const std::size_t staging_bytes = ntohl(request.staging_bytes_be);
        const std::size_t requested_target = ntohl(request.target_node_be);
        if (staging_bytes == 0 || staging_bytes > maximum_bundle_bytes ||
            requested_target != node_id) {
            Check(rdma_reject(resources.id, nullptr, 0), "rdma_reject(session request)");
            std::cerr << "replica_session_rejected node=" << node_id
                      << " requested_target=" << requested_target
                      << " staging_bytes=" << staging_bytes << std::endl;
            continue;
        }

        CreateQueuePair(resources, staging_bytes, true);
        PostReceive(resources);
        MemoryDescriptor descriptor{};
        descriptor.magic_be = htonl(kDescriptorMagic);
        descriptor.version_be = htonl(kProtocolVersion);
        descriptor.address_be = htobe64(static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(resources.data.data())));
        descriptor.rkey_be = htonl(resources.data_mr->rkey);
        descriptor.length_be = htonl(static_cast<std::uint32_t>(staging_bytes));
        descriptor.target_node_be = htonl(static_cast<std::uint32_t>(node_id));
        rdma_conn_param connection{};
        connection.private_data = &descriptor;
        connection.private_data_len = sizeof(descriptor);
        connection.responder_resources = 1;
        connection.initiator_depth = 1;
        connection.retry_count = 7;
        Check(rdma_accept(resources.id, &connection), "rdma_accept");
        rdma_cm_event* established =
            WaitEvent(channel.value, RDMA_CM_EVENT_ESTABLISHED);
        Check(rdma_ack_cm_event(established), "rdma_ack_cm_event(server established)");
        ++session_count;
        std::cout << "replica_session_established node=" << node_id
                  << " session=" << session_count
                  << " staging_bytes=" << staging_bytes << std::endl;

        std::uint64_t expected_sequence = 1;
        bool peer_disconnected = false;
        while (maximum_replications == 0 || attempts < maximum_replications) {
            ibv_wc completion{};
            try {
                completion = WaitCompletion(
                    channel.value, resources.receive_cq, 1, IBV_WC_RECV);
            } catch (const PeerDisconnected&) {
                peer_disconnected = true;
                break;
            }
            ++attempts;

            const std::uint64_t sequence =
                be64toh(resources.receive_control.sequence_be);
            const std::size_t reported_bytes =
                ntohl(resources.receive_control.length_be);
            const std::size_t requested_bucket =
                ntohl(resources.receive_control.bucket_id_be);
            const std::size_t control_target =
                ntohl(resources.receive_control.target_node_be);
            const std::uint32_t requested_checksum =
                ntohl(resources.receive_control.checksum_be);

            TransferStatus status = TransferStatus::kSuccess;
            std::string failure_reason;
            ReplicaBundle decoded;
            std::vector<std::uint8_t> received;
            if (completion.byte_len != sizeof(ControlMessage) ||
                ntohl(resources.receive_control.magic_be) != kDoneMagic ||
                ntohl(resources.receive_control.version_be) != kProtocolVersion ||
                ntohl(resources.receive_control.status_be) != 0) {
                status = TransferStatus::kInvalidControl;
                failure_reason = "invalid completion control message";
            } else if (sequence != expected_sequence) {
                status = TransferStatus::kInvalidSequence;
                failure_reason = "expected sequence " +
                                 std::to_string(expected_sequence) +
                                 ", received " + std::to_string(sequence);
            } else if (reported_bytes == 0 || reported_bytes > staging_bytes) {
                status = TransferStatus::kLengthMismatch;
                failure_reason = "reported bundle length exceeds the staging buffer";
            } else if (control_target != node_id) {
                status = TransferStatus::kWrongTarget;
                failure_reason = "control target does not match this memory node";
            } else {
                received.assign(
                    resources.data.begin(),
                    resources.data.begin() + static_cast<std::ptrdiff_t>(reported_bytes));
                if (ReplicaBundleChecksum(received) != requested_checksum) {
                    status = TransferStatus::kChecksumMismatch;
                    failure_reason = "received replica bytes failed checksum validation";
                } else {
                    try {
                        decoded = DecodeReplicaBundle(received);
                        if (decoded.bucket_id != requested_bucket) {
                            status = TransferStatus::kInvalidBundle;
                            failure_reason = "bundle bucket does not match its control message";
                        } else if (decoded.target_node != node_id) {
                            status = TransferStatus::kWrongTarget;
                            failure_reason = "bundle target does not match this memory node";
                        }
                    } catch (const std::exception& error) {
                        status = TransferStatus::kInvalidBundle;
                        failure_reason = error.what();
                    }
                }
            }

            if (sequence == expected_sequence) {
                ++expected_sequence;
            }
            if (status == TransferStatus::kSuccess) {
                replicas[decoded.bucket_id] = StoredReplica{
                    decoded,
                    std::move(received),
                    ++generation,
                    sequence,
                };
            }

            resources.send_control = ControlMessage{};
            resources.send_control.magic_be = htonl(kAckMagic);
            resources.send_control.version_be = htonl(kProtocolVersion);
            resources.send_control.sequence_be = htobe64(sequence);
            resources.send_control.length_be =
                htonl(static_cast<std::uint32_t>(reported_bytes));
            resources.send_control.bucket_id_be =
                htonl(static_cast<std::uint32_t>(requested_bucket));
            resources.send_control.target_node_be =
                htonl(static_cast<std::uint32_t>(node_id));
            resources.send_control.checksum_be = htonl(requested_checksum);
            resources.send_control.status_be =
                htonl(static_cast<std::uint32_t>(status));
            PostControl(resources, 4);
            WaitCompletion(channel.value, resources.send_cq, 4, IBV_WC_SEND);

            if (status == TransferStatus::kSuccess) {
                const auto& stored = replicas.at(requested_bucket);
                std::cout << "replica_committed bucket=" << requested_bucket
                          << " target_node=" << node_id
                          << " sequence=" << sequence
                          << " records=" << stored.bundle.records.size()
                          << " bytes=" << stored.encoded.size()
                          << " expires_after_window="
                          << stored.bundle.expires_after_window
                          << " generation=" << stored.generation << std::endl;
            } else {
                std::cerr << "replica_rejected bucket=" << requested_bucket
                          << " target_node=" << node_id
                          << " sequence=" << sequence
                          << " status=" << StatusName(status)
                          << " reason=" << failure_reason << std::endl;
            }

            if (maximum_replications == 0 || attempts < maximum_replications) {
                PostReceive(resources);
            }
        }

        if (!peer_disconnected) {
            rdma_cm_event* disconnected =
                WaitEvent(channel.value, RDMA_CM_EVENT_DISCONNECTED);
            Check(rdma_ack_cm_event(disconnected),
                  "rdma_ack_cm_event(server disconnected)");
        }
        std::cout << "replica_session_closed node=" << node_id
                  << " session=" << session_count
                  << " next_sequence=" << expected_sequence << std::endl;
    }

    std::cout << "replica_server_complete node=" << node_id
              << " attempts=" << attempts
              << " sessions=" << session_count
              << " stored_buckets=" << replicas.size() << std::endl;
}

}  // namespace hotbucket
