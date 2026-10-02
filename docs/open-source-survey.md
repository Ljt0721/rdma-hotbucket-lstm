# Open-source implementation survey

Survey updated: 2026-08-26

The aim of this survey is not to find one repository that already implements the thesis. No
reviewed project combines low-frequency future hot-bucket prediction, measured copy cost, and
proactive read replication in a small RDMA key-value prototype. Instead, each project is evaluated
for one part of the implementation.

## Selection criteria

A project is useful only when it satisfies at least one of these needs:

1. It provides a small and understandable RDMA CM/verbs implementation.
2. It shows how a distributed KV system measures demand or changes replication.
3. It supplies a reproducible workload or measurement method.
4. It provides a maintained library for the LSTM experiment.

License, hardware requirements, repository size, and the amount of unrelated code are also
considered. A paper artifact without a clear reuse licence is studied but not copied.

## Candidate projects

| Project | Useful part | Important limitation | Decision |
|---|---|---|---|
| [linux-rdma/rdma-core](https://github.com/linux-rdma/rdma-core) | Official userspace `libibverbs` and `librdmacm` libraries; Soft-RoCE setup | Linux system dependency, not an application framework | Install on test machines; do not vendor |
| [Ljt0721/rdma-basic-tutorial](https://github.com/Ljt0721/rdma-basic-tutorial) | Small CM connection and verbs SEND, RECV, READ, and WRITE examples already understood by the author | Needs resource wrappers, multi-client support, and a licence file | Include as an implementation reference; adapt concepts into `RdmaTransport` |
| [efficient/rdma_bench](https://github.com/efficient/rdma_bench) | RDMA throughput tests, queue-pair setup, completion handling, HERD and MICA examples | Older research code and hardware-specific assumptions | Keep the existing submodule for microbenchmark calibration |
| [utsaslab/dinomo](https://github.com/utsaslab/dinomo) | Monitoring, access-count summaries, movement policy, and replication-factor updates | Full deployment requires Kubernetes and several physical machines | Include as Apache-2.0 reference code; reuse only small ideas and interfaces |
| [dmemsys/FUSEE](https://github.com/dmemsys/FUSEE) | Replicated metadata, client-side memory management, microbenchmarks, and KV operations | Large testbed, older Ubuntu/ConnectX assumptions, and no detected root licence | Study source and paper only; do not copy code |
| [SJTU-IPADS/xstore](https://github.com/SJTU-IPADS/xstore) | Separates learned-model training, serialization, and RDMA lookup; shows model overhead must be measured | Predicts key positions rather than future load; SATA licence | Architecture reference only |
| [dlekkas/netcache](https://github.com/dlekkas/netcache) | Count-Min Sketch, hot-key reports, controller decisions, and LFU-style eviction | Requires P4/BMv2; no detected root licence; detects current demand | Study hotspot measurement, but implement our own bucket counters |
| [brianfrankcooper/YCSB](https://github.com/brianfrankcooper/YCSB) | Standard GET/UPDATE workload mixes, skew controls, and repeatable benchmarking conventions | Java framework is unnecessary for the custom C++ prototype; default traces do not move hotspots | Reproduce the relevant workload rules in our trace generator and add moving hotspots |
| [valkey-io/valkey](https://github.com/valkey-io/valkey) | Mature KV server with experimental Linux RDMA transport | Very large codebase and different data/replication design | Optional external comparison only, not the thesis base |
| [pytorch/pytorch](https://github.com/pytorch/pytorch) | Maintained `torch.nn.LSTM`, data loading, training, and model export | A full framework; inference must remain outside the request path | Install as a Python dependency |
| [malin1993ml/QueryBot5000](https://github.com/malin1993ml/QueryBot5000) | Official QueryBot 5000 artifact; predicts logical SQL arrival rates with forecasting models including an RNN/LSTM path | Query-level relational workload, old dependencies, and no root licence detected | Study its workload-to-sequence design; do not copy code |
| [pytorch/examples](https://github.com/pytorch/examples/tree/main/time_sequence_prediction) | Official minimal example of recurrent next-step sequence prediction | Synthetic sine-wave task without temporal validation or deployment rules | Keep as an API-level reference only |
| [Vicen-te/time-series-benchmark](https://github.com/Vicen-te/time-series-benchmark) | Clear PyTorch LSTM example with temporal splits, training-only standardization, clipping, early stopping, and checkpoint metadata | General time-series benchmark rather than a systems workload artifact | Study its experiment hygiene; implement our own bucket model |
| [unit8co/darts](https://github.com/unit8co/darts) | Ready-made forecasting baselines and common evaluation utilities | Adds abstractions that may hide the actual experiment | Optional fallback for model comparison, not an initial dependency |
| [Nixtla/neuralforecast](https://github.com/Nixtla/neuralforecast) | Maintained neural forecasting implementations | Designed for broader forecasting workloads and may be excessive here | Optional fallback if direct PyTorch experiments are insufficient |
| [google/benchmark](https://github.com/google/benchmark) | Reliable C++ microbenchmark harness | Does not measure distributed end-to-end behaviour | Add only when timing local metadata and routing operations |
| [HdrHistogram/HdrHistogram_c](https://github.com/HdrHistogram/HdrHistogram_c) | Correct latency histogram and percentile recording | Another dependency to manage | Add when physical request latency replaces simulated latency |

## Selected implementation stack

The first complete prototype will use a deliberately small stack:

- **C++17 core:** hash-bucket KV state, clients, routing, replicas, policies, and experiment logs.
- **Python/PyTorch model:** one shared LSTM trained across bucket-window sequences. There will not be
  one model per bucket.
- **rdma-core on Linux:** CM for connection setup and verbs for registered-memory operations.
- **Our own workload generator:** repeatable GET/PUT traces based on common YCSB-style mixes, with
  an added moving-hotspot schedule.
- **Reference artifacts:** `rdma-basic-tutorial`, `rdma_bench`, and DINOMO. They are references, not
  a combined upstream product.

QueryBot 5000, the official PyTorch example, and the independent time-series benchmark are stored
as read-only Git submodules. The reviewed commits are `3e74585`, `acc295d`, and `974a950`
respectively. QueryBot has no detected root licence, while the other two repositories include
licence files. No source from these projects is copied into `ml/`; they are retained so the design
choices can be checked later.

FUSEE, XStore, NetCache, Valkey, Darts, and NeuralForecast remain external study material. This
keeps the code buildable on one computer and prevents the implementation from becoming an attempt
to reproduce several unrelated systems.

## Concrete system boundary

The final system is split into replaceable modules:

```text
fixed request trace
        |
        v
workload replay -> KV and transport -> per-window bucket/node measurements
                                          |
                                          v
                                  LSTM hotspot predictor
                                          |
                                          v
                                cost-aware copy decision
                                          |
                                          v
                            copy bucket + update routing metadata
                                          |
                                          v
                                  split later GET traffic
```

The incoming request trace stays fixed. A policy changes where requests are served, but it does not
change which requests arrive. This avoids training or testing on a future workload rewritten by the
system's own previous actions.

### C++ interfaces to add

1. `ITransport`
   - `LocalTransport` for deterministic simulation and debugging.
   - `SocketTransport` for multi-process testing without RDMA hardware.
   - `RdmaTransport` for CM/verbs and registered bucket memory.
2. `IHotspotPredictor`
   - `RecentWindowPredictor` as the lightweight baseline.
   - `LstmPredictor` loading predictions produced by the Python model.
3. `IReplicationPolicy`
   - no proactive action;
   - reactive severe-hotspot copying;
   - recent-window prediction with the same cost rule;
   - LSTM prediction with the same cost rule.

Keeping prediction and the copy decision separate is important. The model estimates future demand.
The policy decides whether that estimate is strong enough to justify a physical copy.

## Data and model contract

Each bucket produces one row per observation window. The initial fields are:

The trace exporter records identifiers and fixed experiment settings together with `get_count`,
`put_count`, `read_ratio`, `request_share`, and `home_node_logical_load_ratio`. These are logical
demand measurements taken before any policy acts. Future labels are deliberately created later by
the Python dataset builder, so an exported row never contains information from a later window.

The training examples are rolling sequences from the earlier part of a trace. Validation uses the
next time interval, and the test set uses the final unseen interval. Random row splitting is not
allowed because neighbouring time windows would leak information.

The initial model is a small unidirectional LSTM with one layer and 32 hidden units. Six recent
windows predict the next 30-second window, matching the final proposal. One shared model scores all
buckets; it does not create one model per bucket. Its two outputs are severe-hotspot probability
and predicted GET share. Later sensitivity tests may compare other observation intervals, but the
first implementation has one explicit setting instead of silently mixing several definitions.

## Workload construction

The project will not import the full YCSB Java framework. It will implement the small subset needed
for this research:

- read-heavy mix, initially 90% GET and 10% PUT;
- balanced mix, initially 50% GET and 50% PUT;
- uniform and skewed key access;
- stable hotspot, moving hotspot, and burst hotspot schedules;
- the same seed and trace file for every policy.

The trace generator is both the controlled dataset source and the experiment workload. If a real
trace becomes available, it is evaluated separately and is never mixed into training after seeing
its test interval.

## Cost-aware decision

The copy cost will be measured on the current environment instead of being guessed:

```text
copy_cost = bucket_bytes / measured_copy_bandwidth
          + metadata_update_time
          + replica_consistency_time
```

Initially, only read-heavy buckets are eligible. GET requests may be split between the home node and
the temporary replica. PUT requests remain ordered through the home node and update or invalidate
the replica. A copy is made only when the predicted reduction in overloaded service time is larger
than the measured copy cost plus a safety margin for wrong predictions.

## Build sequence

1. **Completed: simulator dataset pipeline.** Export clean traces for six controlled hotspot
   patterns without applying a balancing policy.
2. **Completed: first offline LSTM.** Use time-ordered train/validation/test splitting,
   normalization fitted only on training data, early stopping, checkpoints, and prediction CSV.
3. **Completed: connect prediction to the C++ controller.** Replay unseen traces using frozen
   predictions and compare no action, reactive copying, recent-window prediction, and cost-aware
   LSTM. Copy cost and measured low-frequency inference overhead are included.
4. **Build a multi-process KV prototype.** Separate clients and memory nodes using local or TCP
   transport while retaining the same policy interfaces.
5. **Add RDMA transport.** Start with two Ubuntu VMs and Soft-RoCE. Replace only the transport with
   CM/verbs, then calibrate copy bandwidth and metadata time using `rdma_bench`.
6. **Validate on hardware if available.** Repeat the core experiment on two RDMA-capable machines.
   Cloud hardware is optional and will be used only after its RDMA capability and price are verified.

## Minimum successful FYP result

The minimum complete result is not "LSTM must win." It is a reproducible answer to this question:
under which hotspot duration, prediction accuracy, bucket size, and copy cost does proactive
replication reduce total completion time?

If LSTM is inaccurate or its overhead removes the benefit, the same experiment will show that the
recent-window or reactive policy is more suitable. That remains a valid result because the failure
boundary is measured rather than hidden.
