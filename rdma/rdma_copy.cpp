#include <arpa/inet.h>
#include <endian.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
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

constexpr int kTimeoutMs = 5000;
constexpr std::uint32_t kDoneMagic = 0x48424b54;

struct MemoryDescriptor {
    std::uint64_t address_be{};
    std::uint32_t rkey_be{};
    std::uint32_t length_be{};
};

struct DoneMessage {
    std::uint32_t magic_be{};
    std::uint32_t length_be{};
};

struct Resources {
    rdma_event_channel* channel{};
    rdma_cm_id* listen_id{};
    rdma_cm_id* id{};
    ibv_pd* pd{};
    ibv_cq* cq{};
    ibv_mr* data_mr{};
    ibv_mr* control_mr{};
    std::vector<std::uint8_t> data;
    DoneMessage control{};

    ~Resources() {
        if (id != nullptr && id->qp != nullptr) {
            rdma_destroy_qp(id);
        }
        if (control_mr != nullptr) {
            ibv_dereg_mr(control_mr);
        }
        if (data_mr != nullptr) {
            ibv_dereg_mr(data_mr);
        }
        if (cq != nullptr) {
            ibv_destroy_cq(cq);
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
        throw std::runtime_error(operation + ": " + std::strerror(errno));
    }
}

rdma_cm_event* WaitEvent(rdma_event_channel* channel, rdma_cm_event_type expected) {
    rdma_cm_event* event = nullptr;
    Check(rdma_get_cm_event(channel, &event), "rdma_get_cm_event");
    if (event->event != expected) {
        const std::string actual = rdma_event_str(event->event);
        rdma_ack_cm_event(event);
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
    resources.cq = ibv_create_cq(resources.id->verbs, 16, nullptr, nullptr, 0);
    if (resources.cq == nullptr) {
        throw std::runtime_error("ibv_create_cq failed");
    }

    ibv_qp_init_attr attributes{};
    attributes.qp_type = IBV_QPT_RC;
    attributes.send_cq = resources.cq;
    attributes.recv_cq = resources.cq;
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
    resources.control_mr = ibv_reg_mr(
        resources.pd,
        &resources.control,
        sizeof(resources.control),
        IBV_ACCESS_LOCAL_WRITE);
    if (resources.control_mr == nullptr) {
        throw std::runtime_error("ibv_reg_mr(control) failed");
    }
}

void PostReceive(Resources& resources) {
    ibv_sge scatter{};
    scatter.addr = reinterpret_cast<std::uintptr_t>(&resources.control);
    scatter.length = sizeof(resources.control);
    scatter.lkey = resources.control_mr->lkey;
    ibv_recv_wr request{};
    request.wr_id = 1;
    request.sg_list = &scatter;
    request.num_sge = 1;
    ibv_recv_wr* bad_request = nullptr;
    Check(ibv_post_recv(resources.id->qp, &request, &bad_request), "ibv_post_recv");
}

ibv_wc WaitCompletion(ibv_cq* cq) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kTimeoutMs);
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
            return completion;
        }
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

void VerifyPattern(const std::vector<std::uint8_t>& data, std::size_t bytes) {
    for (std::size_t index = 0; index < bytes; ++index) {
        if (data[index] != PatternByte(index)) {
            throw std::runtime_error("RDMA payload verification failed at byte " +
                                     std::to_string(index));
        }
    }
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
    rdma_ack_cm_event(request);
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
    rdma_ack_cm_event(established);

    const ibv_wc completion = WaitCompletion(resources.cq);
    if (completion.opcode != IBV_WC_RECV ||
        ntohl(resources.control.magic_be) != kDoneMagic) {
        throw std::runtime_error("invalid completion message");
    }
    const std::size_t transferred = ntohl(resources.control.length_be);
    if (transferred > resources.data.size()) {
        throw std::runtime_error("client reported an oversized transfer");
    }
    VerifyPattern(resources.data, transferred);
    std::cout << "verified_bytes=" << transferred << '\n';
    std::cout << "provider=" << ibv_get_device_name(resources.id->verbs->device) << '\n';

    rdma_cm_event* disconnected = WaitEvent(resources.channel, RDMA_CM_EVENT_DISCONNECTED);
    rdma_ack_cm_event(disconnected);
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
              resources.id, nullptr, reinterpret_cast<sockaddr*>(&address), kTimeoutMs),
          "rdma_resolve_addr");
    rdma_cm_event* resolved = WaitEvent(resources.channel, RDMA_CM_EVENT_ADDR_RESOLVED);
    rdma_ack_cm_event(resolved);
    Check(rdma_resolve_route(resources.id, kTimeoutMs), "rdma_resolve_route");
    rdma_cm_event* routed = WaitEvent(resources.channel, RDMA_CM_EVENT_ROUTE_RESOLVED);
    rdma_ack_cm_event(routed);
    CreateQueuePair(resources, bytes, false);

    rdma_conn_param connection{};
    connection.responder_resources = 1;
    connection.initiator_depth = 1;
    connection.retry_count = 7;
    Check(rdma_connect(resources.id, &connection), "rdma_connect");
    rdma_cm_event* established = WaitEvent(resources.channel, RDMA_CM_EVENT_ESTABLISHED);
    if (established->param.conn.private_data == nullptr ||
        established->param.conn.private_data_len < sizeof(MemoryDescriptor)) {
        rdma_ack_cm_event(established);
        throw std::runtime_error("server did not provide a remote memory descriptor");
    }
    MemoryDescriptor descriptor{};
    std::memcpy(&descriptor, established->param.conn.private_data, sizeof(descriptor));
    rdma_ack_cm_event(established);

    const std::uint64_t remote_address = be64toh(descriptor.address_be);
    const std::uint32_t remote_key = ntohl(descriptor.rkey_be);
    const std::size_t remote_length = ntohl(descriptor.length_be);
    if (bytes > remote_length) {
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
    WaitCompletion(resources.cq);
    const auto finished = std::chrono::steady_clock::now();

    resources.control.magic_be = htonl(kDoneMagic);
    resources.control.length_be = htonl(static_cast<std::uint32_t>(bytes));
    ibv_sge done_scatter{};
    done_scatter.addr = reinterpret_cast<std::uintptr_t>(&resources.control);
    done_scatter.length = sizeof(resources.control);
    done_scatter.lkey = resources.control_mr->lkey;
    ibv_send_wr done_request{};
    done_request.wr_id = 3;
    done_request.sg_list = &done_scatter;
    done_request.num_sge = 1;
    done_request.opcode = IBV_WR_SEND;
    done_request.send_flags = IBV_SEND_SIGNALED;
    bad_request = nullptr;
    Check(ibv_post_send(resources.id->qp, &done_request, &bad_request),
          "ibv_post_send(done)");
    WaitCompletion(resources.cq);

    const double milliseconds =
        std::chrono::duration<double, std::milli>(finished - started).count();
    const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::cout << "transferred_bytes=" << bytes << '\n';
    std::cout << "write_completion_ms=" << milliseconds << '\n';
    std::cout << "throughput_mib_per_s=" << mib / (milliseconds / 1000.0) << '\n';
    std::cout << "provider=" << ibv_get_device_name(resources.id->verbs->device) << '\n';
    Check(rdma_disconnect(resources.id), "rdma_disconnect");
}

std::size_t ParseBytes(const char* value) {
    const unsigned long long parsed = std::stoull(value);
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
            const auto port = static_cast<std::uint16_t>(argc > 2 ? std::stoul(argv[2]) : 7471);
            const std::size_t bytes = argc > 3 ? ParseBytes(argv[3]) : 4U * 1024U * 1024U;
            RunServer(port, bytes);
        } else if (mode == "client") {
            if (argc < 3) {
                throw std::invalid_argument("client mode requires the server IPv4 address");
            }
            const auto port = static_cast<std::uint16_t>(argc > 3 ? std::stoul(argv[3]) : 7471);
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
