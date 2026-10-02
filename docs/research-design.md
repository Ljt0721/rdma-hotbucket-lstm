# Research design implemented by this repository

The experiment replays the same generated request demand for every policy. A policy can change
where requests are served, but it cannot change the original GET/PUT trace. This prevents an action
from rewriting the future test workload and keeps policy comparisons fair.

The implementation contains four policies:

1. `no-action`: keep each bucket on its home node.
2. `reactive`: copy the hottest bucket after the previous window crossed a severe threshold.
3. `recent-window`: extrapolate the last two windows and copy only when estimated benefit is larger
   than estimated copy cost.
4. `lstm`: read frozen next-window probabilities and GET estimates, discount predicted benefit by
   model confidence, and apply the same copy-cost rule.

The recent-window policy is a simple baseline. The LSTM predicts future bucket demand, while the
cost-aware rule decides whether to create a replica. The model cannot directly copy data.

Each CSV row is one bucket in one time window. The implemented LSTM inputs are GET count, PUT count,
read ratio, request share, and logical home-node load. Six past windows predict the next window's
GET share and severe-hotspot label. Training, tuning, and test data are split in time order.

The first 85% of every formal trace is replayed as action-free history. Only the final 15% permits
policy decisions and contributes to completion-time results. This gives every policy the same
history and prevents pre-test copies from changing the starting state. LSTM inference is measured
separately and its p95 CPU time is added to every LSTM evaluation window.

The current queue and copy formulas are parameterized simulation models. They establish the
experiment pipeline; they are not evidence of real RDMA performance. Physical claims require
calibration against RDMA CM/verbs microbenchmarks on Linux with RDMA hardware or Soft-RoCE.

The current entity-placement demonstration uses ten battlefield objects and five logical memory
nodes. Every entity contains variable-size state attributes. The C++ backend serializes each record,
hashes `EntityID` to a bucket, maps the bucket to a home node, and stores the bytes in that node's
contiguous memory vector with 64-byte alignment. The dashboard renders the resulting node, offset,
serialized size, aligned allocation, and attributes. Workload generation uses the same ten entity
IDs, so a displayed hot bucket refers to data that was actually placed in the simulated pool.

When a policy approves replication, the backend copies every primary entity byte range in that
bucket into a free aligned range on the selected node. The routing simulator and byte-addressed
memory pool use the same target and expiry window. GET requests can then be split between the home
node and replica. PUT requests count one update at each active location. At TTL expiry, routing
metadata is removed, the copied bytes are zeroed, and the freed range can be reused. Copy time is
estimated from the configured logical bucket payload or the actual aligned demo bytes, whichever is
larger, plus the configured metadata cost. The visible entity record is only a representative part
of a logical bucket and must not make a multi-megabyte copy appear free.
