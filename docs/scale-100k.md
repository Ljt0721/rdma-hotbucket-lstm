# 100,000-Entity Scale Milestone

This milestone checks that entity count is no longer limited by dashboard rendering or quadratic
initial placement. It is a scalability smoke test, not a final throughput result.

## Configuration

- 100,000 durable entity records
- 5 logical memory nodes
- 64 hash buckets
- 10,000 entities at most per visual memory block
- 1,000-allocation threshold for switching from detailed to aggregate placement
- 12 windows, 4,000 requests per window for the policy smoke test

## Local result

The HBKV generator produced 100,000 records in 339.0 ms. The file size was 52,588,932 bytes.
Initial allocation completed in 363.4 ms and used 51.27 MiB from a 97.66 MiB simulated pool.

The five node counts were 20,312, 20,313, 20,313, 20,313, and 18,749. The difference follows the
mapping of 64 buckets onto five nodes. The placement was represented by 14 blocks. No block held
more than 10,000 entities, and the complete placement JSON was 3,077 bytes.

A 12-window recent-window run completed its host-side program execution in 387.3 ms. It applied
four bucket-copy decisions. At the peak, 3,126 replica entity records were present. The largest
aggregate memory snapshot was 3,404 bytes.

## Real RDMA smoke test

The same 100,000-record HBKV snapshot was uploaded to the compute VM and loaded by the Linux RDMA
simulator. Five persistent replica services ran on the memory VM. A 12-window recent-window run
completed with four successful `IBV_WR_RDMA_WRITE` operations and no protocol error. Individual
bucket bundles were approximately 0.86 to 0.90 MB. Soft-RoCE WRITE time was approximately 50 to
78 ms, with acknowledgement included after each write.

The cost-aware LSTM path also completed at this scale. It used six previous windows, copied four
predicted buckets in windows 6, 9, 10, and 11, and reported no RDMA or sequence error. These are
correctness and feasibility results on virtual Soft-RoCE, not physical-RNIC performance claims.

## CRUD boundary

The Node API successfully created, read, updated, and deleted a temporary Tank while the 100,000
record store was active. On this machine, one read took about 0.4 seconds, while create and update
took about 1.4 to 1.6 seconds because they rewrote the complete HBKV snapshot.

This is acceptable for experiment setup and durable inspection, but it is not a claim of a
production KV write path. The performance experiment uses in-memory request processing. A later
milestone should add an index and append-only update log, then connect real in-memory CRUD latency
to the workload measurements.
