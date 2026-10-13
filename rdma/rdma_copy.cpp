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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kResolveTimeoutMs = 5000;
constexpr int kCmEventTimeoutMs = 30000;
constexpr int kCompletionTimeoutMs = 60000;
constexpr std::uint16_t kDefaultPort = 7471;
constexpr std::uint32_t kDoneMagic = 0x48424b54;
constexpr std::uint32_t kAckMagic = 0x48424143;

enum class TransferStatus : std::uint32_t {
    kSuccess = 0,
    kInvalidControlMessage = 1,
    kLengthMismatch = 2,
    kPayloadMismatch = 3,
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
};

static_assert(sizeof(MemoryDescriptor) == 16, "unexpected memory descriptor layout");
static_assert(sizeof(ControlMessage) == 12, "unexpected control-message layout");

struct Resources {
    rdma_event_channel* channel{};
    rdma_cm_id* listen_id{};
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

    ~Resources() {
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
        if (listen_id != nullptr) {
            rdma_destroy_id(listen_id);
        }
        if (channel != nullptr) {
            rdma_destroy_event_channel(channel);
        }
    }
};

void Check(int result, const std::string& operation) {
    if (result != 0) {
        const int error_code = result > 0 ? result : errno;
        throw std::runtime_error(operation + ": " + std::strerror(error_code));
    }
}

rdma_cm_event* WaitEvent(rdma_event_channel* channel, rdma_cm_event_type expected) {
    pollfd descriptor{};
    descriptor.fd = channel->fd;
    descriptor.events = POLLIN;
    int poll_result = 0;
    do {
        poll_result = poll(&descriptor, 1, kCmEventTimeoutMs);
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
        Check(rdma_ack_cm_event(event), "rdma_ack_cm_event(unexpected)");
        throw std::runtime_error(
            "expected CM event " + std::string(rdma_event_str(expected)) + ", received " + actual);
    }
    return event;
}

void CreateQueuePair(Resources& resources, std::size_t bytes, bool remote_target) {
    resources.pd = ibv_alloc_pd(resources.id->verbs);
    if (resources.pd == nullptr) {
        throw std::runtime_error("ibv_alloc_pd failed");
    }
    resources.send_cq = ibv_create_cq(resources.id->verbs, 16, nullptr, nullptr, 0);
    if (resources.send_cq == nullptr) {
        throw std::runtime_error("ibv_create_cq(send) failed");
    }
    resources.receive_cq = ibv_create_cq(resources.id->verbs, 16, nullptr, nullptr, 0);
    if (resources.receive_cq == nullptr) {
        throw std::runtime_error("ibv_create_cq(receive) failed");
    }

    ibv_qp_init_attr attributes{};
    attributes.qp_type = IBV_QPT_RC;
    attributes.send_cq = resources.send_cq;
    attributes.recv_cq = resources.receive_cq;
    attributes.cap.max_send_wr = 8;
    attributes.cap.max_recv_wr = 8;
    attributes.cap.max_send_sge = 1;
    attributes.cap.max_recv_sge = 1;
    Check(rdma_create_qp(resources.id, resources.pd, &attributes), "rdma_create_qp");

    resources.data.resize(bytes);
    int data_access = IBV_ACCESS_LOCAL_WRITE;
    if (remote_target) {
        data_access |= IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    }
    resources.data_mr = ibv_reg_mr(
        resources.pd, resources.data.data(), resources.data.size(), data_access);
    if (resources.data_mr == nullptr) {
        throw std::runtime_error("ibv_reg_mr(data) failed");
    }
    resources.send_control_mr = ibv_reg_mr(
        resources.pd,
        &resources.send_control,
        sizeof(resources.send_control),
        IBV_ACCESS_LOCAL_WRITE);
    if (resources.send_control_mr == nullptr) {
        throw std::runtime_error("ibv_reg_mr(send control) failed");
    }
    resources.receive_control_mr = ibv_reg_mr(
        resources.pd,
        &resources.receive_control,
        sizeof(resources.receive_control),
        IBV_ACCESS_LOCAL_WRITE);
    if (resources.receive_control_mr == nullptr) {
        throw std::runtime_error("ibv_reg_mr(receive control) failed");
    }
}

void PostReceive(Resources& resources) {
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

void PostControl(Resources& resources, std::uint64_t work_request_id) {
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
    Check(ibv_post_send(resources.id->qp, &request, &bad_request), "ibv_post_send(control)");
}

void ThrowIfConnectionClosed(rdma_event_channel* channel) {
    if (channel == nullptr) {
        return;
    }
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
    const rdma_cm_event_type event_type = event->event;
    const int event_status = event->status;
    Check(rdma_ack_cm_event(event), "rdma_ack_cm_event(connection event)");
    if (event_type == RDMA_CM_EVENT_DISCONNECTED) {
        throw std::runtime_error("peer disconnected before the RDMA operation completed");
    }
    throw std::runtime_error(
        "unexpected CM event while waiting for completion: " +
        std::string(rdma_event_str(event_type)) + ", status=" +
        std::to_string(event_status));
}

ibv_wc WaitCompletion(
    ibv_cq* cq,
    std::uint64_t expected_wr_id,
    ibv_wc_opcode expected_opcode,
    rdma_event_channel* channel = nullptr) {
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
            if (completion.wr_id != expected_wr_id || completion.opcode != expected_opcode) {
                throw std::runtime_error(
                    "unexpected work completion: wr_id=" + std::to_string(completion.wr_id) +
                    ", opcode=" + std::to_string(completion.opcode));
            }
            return completion;
        }
        ThrowIfConnectionClosed(channel);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    throw std::runtime_error("completion queue timeout");
}

std::uint8_t PatternByte(std::size_t index) {
    return static_cast<std::uint8_t>((index * 31U + 7U) & 0xffU);
}

void FillPattern(std::vector<std::uint8_t>& data) {
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = PatternByte(index);
    }
}

std::size_t FirstPatternMismatch(const std::vector<std::uint8_t>& data, std::size_t bytes) {
    for (std::size_t index = 0; index < bytes; ++index) {
        if (data[index] != PatternByte(index)) {
            return index;
        }
    }
    return bytes;
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

void RunServer(std::uint16_t port, std::size_t bytes) {
    Resources resources;
    resources.channel = rdma_create_event_channel();
    if (resources.channel == nullptr) {
        throw std::runtime_error("rdma_create_event_channel failed");
    }
    Check(rdma_create_id(
              resources.channel, &resources.listen_id, nullptr, RDMA_PS_TCP),
          "rdma_create_id(listener)");
    auto address = Address("", port, true);
    Check(rdma_bind_addr(
              resources.listen_id, reinterpret_cast<sockaddr*>(&address)),
          "rdma_bind_addr");
    Check(rdma_listen(resources.listen_id, 4), "rdma_listen");
    std::cout << "listening_port=" << port << '\n';

    rdma_cm_event* request = WaitEvent(
        resources.channel, RDMA_CM_EVENT_CONNECT_REQUEST);
    resources.id = request->id;
    Check(rdma_ack_cm_event(request), "rdma_ack_cm_event(connect request)");
    CreateQueuePair(resources, bytes, true);
    PostReceive(resources);

    MemoryDescriptor descriptor{};
    descriptor.address_be = htobe64(
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(resources.data.data())));
    descriptor.rkey_be = htonl(resources.data_mr->rkey);
    descriptor.length_be = htonl(static_cast<std::uint32_t>(resources.data.size()));
    rdma_conn_param connection{};
    connection.private_data = &descriptor;
    connection.private_data_len = sizeof(descriptor);
    connection.responder_resources = 1;
    connection.initiator_depth = 1;
    connection.retry_count = 7;
    Check(rdma_accept(resources.id, &connection), "rdma_accept");
    rdma_cm_event* established = WaitEvent(resources.channel, RDMA_CM_EVENT_ESTABLISHED);
    Check(rdma_ack_cm_event(established), "rdma_ack_cm_event(server established)");

    const ibv_wc completion =
        WaitCompletion(resources.receive_cq, 1, IBV_WC_RECV, resources.channel);
    const std::size_t transferred = ntohl(resources.receive_control.length_be);
    TransferStatus status = TransferStatus::kSuccess;
    std::string validation_error;
    if (completion.byte_len != sizeof(ControlMessage) ||
        ntohl(resources.receive_control.magic_be) != kDoneMagic) {
        status = TransferStatus::kInvalidControlMessage;
        validation_error = "invalid completion message";
    } else if (transferred != resources.data.size()) {
        status = TransferStatus::kLengthMismatch;
        validation_error =
            "client reported " + std::to_string(transferred) + " bytes; expected " +
            std::to_string(resources.data.size());
    } else {
        const std::size_t mismatch = FirstPatternMismatch(resources.data, transferred);
        if (mismatch != transferred) {
            status = TransferStatus::kPayloadMismatch;
            validation_error = "RDMA payload verification failed at byte " +
                               std::to_string(mismatch);
        }
    }

    resources.send_control.magic_be = htonl(kAckMagic);
    resources.send_control.length_be = htonl(static_cast<std::uint32_t>(resources.data.size()));
    resources.send_control.status_be = htonl(static_cast<std::uint32_t>(status));
    PostControl(resources, 4);
    WaitCompletion(resources.send_cq, 4, IBV_WC_SEND, resources.channel);
    if (status != TransferStatus::kSuccess) {
        throw std::runtime_error(validation_error);
    }
    std::cout << "verified_bytes=" << transferred << '\n';
    std::cout << "provider=" << ibv_get_device_name(resources.id->verbs->device) << '\n';

    rdma_cm_event* disconnected = WaitEvent(resources.channel, RDMA_CM_EVENT_DISCONNECTED);
    Check(rdma_ack_cm_event(disconnected), "rdma_ack_cm_event(disconnected)");
}

void RunClient(const std::string& server_ip, std::uint16_t port, std::size_t bytes) {
    Resources resources;
    resources.channel = rdma_create_event_channel();
    if (resources.channel == nullptr) {
        throw std::runtime_error("rdma_create_event_channel failed");
    }
    Check(rdma_create_id(resources.channel, &resources.id, nullptr, RDMA_PS_TCP),
          "rdma_create_id(client)");
    auto address = Address(server_ip, port, false);
    Check(rdma_resolve_addr(
              resources.id, nullptr, reinterpret_cast<sockaddr*>(&address), kResolveTimeoutMs),
          "rdma_resolve_addr");
    rdma_cm_event* resolved = WaitEvent(resources.channel, RDMA_CM_EVENT_ADDR_RESOLVED);
    Check(rdma_ack_cm_event(resolved), "rdma_ack_cm_event(address resolved)");
    Check(rdma_resolve_route(resources.id, kResolveTimeoutMs), "rdma_resolve_route");
    rdma_cm_event* routed = WaitEvent(resources.channel, RDMA_CM_EVENT_ROUTE_RESOLVED);
    Check(rdma_ack_cm_event(routed), "rdma_ack_cm_event(route resolved)");
    CreateQueuePair(resources, bytes, false);
    PostReceive(resources);

    rdma_conn_param connection{};
    connection.responder_resources = 1;
    connection.initiator_depth = 1;
    connection.retry_count = 7;
    Check(rdma_connect(resources.id, &connection), "rdma_connect");
    rdma_cm_event* established = WaitEvent(resources.channel, RDMA_CM_EVENT_ESTABLISHED);
    if (established->param.conn.private_data == nullptr ||
        established->param.conn.private_data_len < sizeof(MemoryDescriptor)) {
        Check(rdma_ack_cm_event(established), "rdma_ack_cm_event(invalid established)");
        throw std::runtime_error("server did not provide a remote memory descriptor");
    }
    MemoryDescriptor descriptor{};
    std::memcpy(&descriptor, established->param.conn.private_data, sizeof(descriptor));
    Check(rdma_ack_cm_event(established), "rdma_ack_cm_event(client established)");

    const std::uint64_t remote_address = be64toh(descriptor.address_be);
    const std::uint32_t remote_key = ntohl(descriptor.rkey_be);
    const std::size_t remote_length = ntohl(descriptor.length_be);
    if (bytes > remote_length) {
        Check(rdma_disconnect(resources.id), "rdma_disconnect(oversized transfer)");
        throw std::runtime_error("requested transfer exceeds the server memory region");
    }
    FillPattern(resources.data);

    ibv_sge scatter{};
    scatter.addr = reinterpret_cast<std::uintptr_t>(resources.data.data());
    scatter.length = static_cast<std::uint32_t>(bytes);
    scatter.lkey = resources.data_mr->lkey;
    ibv_send_wr write_request{};
    write_request.wr_id = 2;
    write_request.sg_list = &scatter;
    write_request.num_sge = 1;
    write_request.opcode = IBV_WR_RDMA_WRITE;
    write_request.send_flags = IBV_SEND_SIGNALED;
    write_request.wr.rdma.remote_addr = remote_address;
    write_request.wr.rdma.rkey = remote_key;
    ibv_send_wr* bad_request = nullptr;
    const auto started = std::chrono::steady_clock::now();
    Check(ibv_post_send(resources.id->qp, &write_request, &bad_request),
          "ibv_post_send(RDMA_WRITE)");
    WaitCompletion(resources.send_cq, 2, IBV_WC_RDMA_WRITE, resources.channel);
    const auto finished = std::chrono::steady_clock::now();

    resources.send_control.magic_be = htonl(kDoneMagic);
    resources.send_control.length_be = htonl(static_cast<std::uint32_t>(bytes));
    resources.send_control.status_be = htonl(0);
    PostControl(resources, 3);
    WaitCompletion(resources.send_cq, 3, IBV_WC_SEND, resources.channel);

    const ibv_wc acknowledgement =
        WaitCompletion(resources.receive_cq, 1, IBV_WC_RECV, resources.channel);
    const auto acknowledgement_status = static_cast<TransferStatus>(
        ntohl(resources.receive_control.status_be));
    if (acknowledgement.byte_len != sizeof(ControlMessage) ||
        ntohl(resources.receive_control.magic_be) != kAckMagic ||
        ntohl(resources.receive_control.length_be) != bytes ||
        acknowledgement_status != TransferStatus::kSuccess) {
        rdma_disconnect(resources.id);
        throw std::runtime_error(
            "server rejected or did not verify the RDMA payload; status=" +
            std::to_string(static_cast<std::uint32_t>(acknowledgement_status)));
    }

    const double milliseconds =
        std::chrono::duration<double, std::milli>(finished - started).count();
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::cout << "transferred_bytes=" << bytes << '\n';
    std::cout << "write_completion_ms=" << milliseconds << '\n';
    std::cout << "throughput_mib_per_s=" << mib / (milliseconds / 1000.0) << '\n';
    std::cout << "provider=" << ibv_get_device_name(resources.id->verbs->device) << '\n';
    Check(rdma_disconnect(resources.id), "rdma_disconnect");
}

unsigned long long ParseUnsigned(const char* value, const std::string& name) {
    const std::string text = value == nullptr ? "" : value;
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument(name + " must be a positive integer");
    }
    std::size_t consumed = 0;
    const unsigned long long parsed = std::stoull(text, &consumed, 10);
    if (consumed != text.size()) {
        throw std::invalid_argument(name + " must contain digits only");
    }
    return parsed;
}

std::uint16_t ParsePort(const char* value) {
    const unsigned long long parsed = ParseUnsigned(value, "port");
    if (parsed == 0 || parsed > UINT16_MAX) {
        throw std::invalid_argument("port must be between 1 and 65535");
    }
    return static_cast<std::uint16_t>(parsed);
}

std::size_t ParseBytes(const char* value) {
    const unsigned long long parsed = ParseUnsigned(value, "bytes");
    if (parsed == 0 || parsed > UINT32_MAX) {
        throw std::invalid_argument("bytes must be between 1 and UINT32_MAX");
    }
    return static_cast<std::size_t>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "usage: hotbucket_rdma_copy server [port] [bytes]\n"
                      << "       hotbucket_rdma_copy client <server-ip> [port] [bytes]\n";
            return 2;
        }
        const std::string mode = argv[1];
        if (mode == "server") {
            if (argc > 4) {
                throw std::invalid_argument("server mode accepts at most port and bytes");
            }
            const std::uint16_t port = argc > 2 ? ParsePort(argv[2]) : kDefaultPort;
            const std::size_t bytes = argc > 3 ? ParseBytes(argv[3]) : 4U * 1024U * 1024U;
            RunServer(port, bytes);
        } else if (mode == "client") {
            if (argc < 3) {
                throw std::invalid_argument("client mode requires the server IPv4 address");
            }
            if (argc > 5) {
                throw std::invalid_argument(
                    "client mode accepts only server IPv4 address, port, and bytes");
            }
            const std::uint16_t port = argc > 3 ? ParsePort(argv[3]) : kDefaultPort;
            const std::size_t bytes = argc > 4 ? ParseBytes(argv[4]) : 4U * 1024U * 1024U;
            RunClient(argv[2], port, bytes);
        } else {
            throw std::invalid_argument("mode must be server or client");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
