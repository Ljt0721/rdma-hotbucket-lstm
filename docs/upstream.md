# Upstream implementation studied

This project uses the following repository as an implementation reference:

- [efficient/rdma_bench](https://github.com/efficient/rdma_bench), especially its HERD key-value
  cache, RDMA READ/WRITE throughput microbenchmarks, queue-pair setup helpers, and completion
  handling. The repository is licensed under Apache License 2.0.

It is included as a Git submodule under `third_party/rdma_bench`. No upstream source is presented as
original work. Our bucket model, workload generator, prediction interface, replication policy, and
experimental logging are developed separately for this FYP.
