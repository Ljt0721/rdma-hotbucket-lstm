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

Additional paper artifacts are linked and evaluated in [open-source-survey.md](open-source-survey.md).
FUSEE, XStore, and NetCache are not vendored because their detected licensing or implementation
scope is unsuitable for direct reuse here.

Our bucket model, moving-hotspot trace generator, prediction interface, cost-aware policy, and
experimental logging are developed separately for this FYP.
