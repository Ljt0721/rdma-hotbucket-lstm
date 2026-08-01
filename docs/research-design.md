# Research design implemented by this repository

The experiment replays the same generated request demand for every policy. A policy can change
where requests are served, but it cannot change the original GET/PUT trace. This prevents an action
from rewriting the future test workload and keeps policy comparisons fair.

The first implementation contains three policies:

1. `no-action`: keep each bucket on its home node.
2. `reactive`: copy the hottest bucket after the previous window crossed a severe threshold.
3. `recent-window`: extrapolate the last two windows and copy only when estimated benefit is larger
   than estimated copy cost.

The recent-window policy is a simple baseline, not the proposed LSTM result. The LSTM module will
later implement the same predictor/decision boundary: the model predicts future bucket demand,
while the cost-aware rule decides whether to create a replica.

Each CSV row is one bucket in one time window. The main future LSTM inputs will be recent GET/PUT
counts, read ratio, latency, replica count, and node load. The target will be the bucket's future
request count or severe-hotspot label. Training, tuning, and test data will be split in time order.

The current queue and copy formulas are parameterized simulation models. They establish the
experiment pipeline; they are not evidence of real RDMA performance. Physical claims require
calibration against RDMA CM/verbs microbenchmarks on Linux with RDMA hardware or Soft-RoCE.
