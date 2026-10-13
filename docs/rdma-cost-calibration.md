# Soft-RoCE replica-cost calibration

Run date: 2026-10-13

## Purpose

The early simulator assumed a 4 MiB bucket could be copied at 1.5 MiB/ms plus 0.08 ms of fixed
metadata cost. That setting was deliberately provisional. This experiment measures the project's
actual persistent-session protocol so later simulations count a defensible local-VM copy cost.

The measurement is not a generic bandwidth probe. Every sample encodes a valid versioned replica
bundle, performs one-sided `IBV_WR_RDMA_WRITE`, sends the sequence-numbered DONE message, verifies
length, checksum, bundle structure and target node, commits the replica, and waits for the ACK.

## Setup

- VMware compute client: `compute-client`
- VMware memory service: `memory-node`
- Transport: Soft-RoCE/RXE over `ens37`
- One persistent RC session per payload size
- Five warm-up copies followed by 30 measured copies
- Payload sizes: 4 KiB, 64 KiB, 256 KiB, 1 MiB, and 4 MiB
- Connection setup is reported separately and is not charged to each copy

## Results

| Payload | RDMA WRITE p50 | RDMA WRITE p95 | Commit ACK p50 | Commit ACK p95 |
|---:|---:|---:|---:|---:|
| 4 KiB | 1.14 ms | 1.86 ms | 2.67 ms | 4.38 ms |
| 64 KiB | 2.82 ms | 4.94 ms | 6.09 ms | 7.60 ms |
| 256 KiB | 10.22 ms | 21.97 ms | 15.41 ms | 27.17 ms |
| 1 MiB | 58.30 ms | 70.97 ms | 69.70 ms | 82.72 ms |
| 4 MiB | 299.73 ms | 322.53 ms | 338.64 ms | 362.31 ms |

The median end-to-end model, anchored at the smallest payload, is:

```text
copy_cost_ms = 2.337 + bucket_size_mib / 0.012051
```

The fit has `R^2 = 0.996`. The corresponding effective rate is 12.05 MiB/s. The conservative p95
model uses 4.026 ms fixed cost and 0.011244 MiB/ms.

These RXE values are much slower than the old placeholder. The old model assigned about 2.75 ms
to a 4 MiB copy, while the measured median commit time is 338.64 ms. This confirms that the
replication cost can change the policy conclusion and must not be ignored.

## Calibration-aware policy replay

The p50 model was applied without changing the frozen `0.40/0.0` LSTM gates. All four policies
replayed the same 48 confirmation traces.

| Policy | Mean completion time | Mean change from no action | Mean copies |
|---|---:|---:|---:|
| No action | 48,236.96 ms | 0.00% | 0.00 |
| Reactive | 18,511.10 ms | 56.52% faster | 8.38 |
| Recent window | 17,985.52 ms | 56.46% faster | 8.27 |
| Cost-aware LSTM | 18,442.59 ms | 53.58% faster | 7.19 |

LSTM remained faster than no action on 45 traces and tied on three. Against recent-window
prediction, however, it was 11.45% slower on the mean relative comparison; the paired bootstrap
95% interval was from 29.92% slower to 0.36% slower. It copied less often and had a 98.84% useful
copy rate, but covered only 75.68% of severe bucket-windows versus 81.29% for recent-window.

This is an exploratory recalculation, not the final thesis result. The confirmation traces were
already inspected in earlier development, the service-time model is still simulated, and RXE is a
software implementation rather than a physical RNIC. The next model or policy change must use
training/validation data, then be evaluated on a newly generated seed range.

## Reproduction

```powershell
python scripts/run_rdma_calibration.py

python scripts/run_formal_evaluation.py `
  --manifest data/traces/final-confirmation/raw/manifest.csv `
  --prediction-file data/predictions/final-confirmation.csv `
  --rdma-calibration results/rdma-calibration/summary.json `
  --output-dir results/calibrated-final-confirmation
```

Use `--calibration-percentile p95` for the conservative cost model.
