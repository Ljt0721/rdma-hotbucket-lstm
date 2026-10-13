# Upstream implementations studied

The Git submodules in `third_party/` are kept as implementation references. They are not linked into
the current executable, and no upstream source is presented as original work.

- [efficient/rdma_bench](https://github.com/efficient/rdma_bench): RDMA READ/WRITE throughput
  microbenchmarks, queue-pair setup, completion handling, HERD, and MICA examples. Its README
  includes Apache License 2.0 terms.
- [Ljt0721/rdma-basic-tutorial](https://github.com/Ljt0721/rdma-basic-tutorial): the author's earlier
  CM and verbs learning examples. The repository currently has no explicit licence file, so it is
  used as a reference owned by the same author and is not copied into third-party distributions.
- [utsaslab/dinomo](https://github.com/utsaslab/dinomo): monitoring, access-count aggregation,
  movement policy, and replication-factor update logic. It is distributed under Apache License 2.0.
- [malin1993ml/QueryBot5000](https://github.com/malin1993ml/QueryBot5000): official QueryBot 5000
  workload-forecasting artifact. It is retained to study query-arrival sequence construction. No
  root licence file was detected, so its source is not copied.
- [pytorch/examples](https://github.com/pytorch/examples): official recurrent sequence-prediction
  example used to confirm the basic PyTorch API. The repository uses the BSD 3-Clause License.
- [Vicen-te/time-series-benchmark](https://github.com/Vicen-te/time-series-benchmark): independent
  MIT-licensed example used to check temporal splitting, training-only normalization, clipping,
  early stopping, and checkpoint metadata.

Additional paper artifacts are linked and evaluated in [open-source-survey.md](open-source-survey.md).
FUSEE, XStore, and NetCache are not vendored because their detected licensing or implementation
scope is unsuitable for direct reuse here.

The software-RDMA path also follows the official
[`linux-rdma/rdma-core`](https://github.com/linux-rdma/rdma-core) RXE instructions and its
`rdma_cm` API documentation. [`linux-rdma/perftest`](https://github.com/linux-rdma/perftest) is used
as the independent fabric check before running project code. These repositories are linked rather
than vendored because the project only needs their installed libraries and command-line tools.

Our bucket model, moving-hotspot trace generator, chronological dataset builder, shared LSTM,
prediction interface, cost-aware policy, and experimental logging are developed separately for
this FYP.
