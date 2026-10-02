# Software RDMA test path

## What this environment tests

Linux RXE implements RoCE in software over an ordinary Ethernet interface. Applications still use
`rdma_cm` for connection setup and `libibverbs` for protection domains, memory registration, queue
pairs, work requests, and completion queues. The same `hotbucket_rdma_copy` source can therefore run
with RXE now and a physical RNIC later. No Mellanox-specific library is linked.

RXE is suitable for API correctness, multi-node protocol development, and measuring the software
path. It does not reproduce RNIC offload, PCIe behaviour, or hardware RDMA latency. Final performance
claims must be repeated on physical RDMA hardware.

## Recommended physical setup

Use two Ubuntu 24.04 virtual machines on one host-only or bridged virtual network. Give each VM two
virtual CPUs, at least 4 GiB RAM, and a fixed IPv4 address. A useful first layout is:

| Role | Address | Process |
|---|---|---|
| Memory node | `192.168.100.10` | RDMA copy server |
| Compute client | `192.168.100.11` | RDMA copy client |

The current Microsoft WSL2 kernel on the development machine contains `rdma_cm` but not the
`rdma_rxe` module. A normal Ubuntu VM is therefore the shortest reproducible route. A custom WSL
kernel with `CONFIG_RDMA_RXE` is possible, but adds kernel maintenance that is unrelated to the FYP.

## Install and create the RXE device

Run these commands inside both VMs from the repository root:

```bash
sudo bash scripts/install_rdma_dependencies.sh
ip -brief link
sudo bash scripts/setup_soft_roce.sh enp0s8
rdma link show
ibv_devices
```

Replace `enp0s8` with the interface carrying the VM-to-VM traffic. The setup script loads
`rdma_rxe` and executes the standard command:

```bash
rdma link add rxe_enp0s8 type rxe netdev enp0s8
```

## Verify the software fabric

Before testing project code, verify the provider with `perftest`.

On the memory-node VM:

```bash
ib_write_bw -d rxe_enp0s8 --report_gbits
```

On the compute-client VM:

```bash
ib_write_bw -d rxe_enp0s8 --report_gbits 192.168.100.10
```

This isolates RXE or network configuration faults from application faults.

## Build and run the project probe

```bash
cmake -S . -B build-rdma -DHOTBUCKET_ENABLE_RDMA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-rdma -j
```

Start a 4 MiB registered memory region on the memory node:

```bash
./build-rdma/hotbucket_rdma_copy server 7471 4194304
```

Write the same 4 MiB pattern from the compute client:

```bash
./build-rdma/hotbucket_rdma_copy client 192.168.100.10 7471 4194304
```

The program uses CM address and route resolution, an RC queue pair, registered memory, a one-sided
`IBV_WR_RDMA_WRITE`, completion polling, and SEND/RECV control messages. The server checks the exact
length and every payload byte, then returns an ACK or NACK. The client reports success and throughput
only after receiving the ACK. CM and completion waits have finite timeouts, and an early peer
disconnect terminates the pending operation instead of leaving the process blocked.

For memory-safety debugging, use a separate sanitizer build on both VMs:

```bash
cmake -S . -B build-rdma-asan -DHOTBUCKET_ENABLE_RDMA=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-rdma-asan -j
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  ./build-rdma-asan/hotbucket_rdma_copy server 7471 4194304
```

Run the matching sanitizer client on the other VM. Test equal sizes for success and deliberately
different sizes to confirm that both sides reject a partial or oversized transfer.

## Relation to the simulator

This probe is the first transport milestone, not yet the complete distributed KV store. Its measured
bandwidth and metadata/control latency can replace the simulator's assumed copy parameters. The next
transport step is to place the same operations behind a `BucketCopyTransport` interface so a policy
decision can choose either simulated copy or RDMA WRITE without changing the LSTM or replication
logic.
