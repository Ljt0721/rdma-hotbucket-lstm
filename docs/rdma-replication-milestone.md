# RDMA bucket-replication milestone

Run date: 2026-10-04

## What is now implemented

The transport no longer copies a synthetic byte pattern. `EntityMemoryCluster` creates a versioned
bundle from every primary allocation in the selected bucket. The bundle records the bucket and
target node, replica expiry window, entity IDs, source nodes, serialized lengths, aligned lengths,
the actual allocation bytes, and a checksum.

The compute process sends the bundle to a logical memory-node service through a one-sided
`IBV_WR_RDMA_WRITE`. The service checks the control message, byte length, checksum, bundle structure,
bucket ID, and target node. It moves the validated registered buffer into its replica store before
returning an ACK. The simulator updates its read-routing state only after that ACK.

## Verification performed

- A one-record 576-byte bucket and a two-record 984-byte bucket were transferred and decoded.
- Repeating the same bucket produced generations 1 and 2 while retaining one current stored copy.
- Connecting to the wrong target node produced `RDMA_CM_EVENT_REJECTED` and no replica commit.
- ASan and UBSan reported no memory or undefined-behaviour errors.
- Valgrind reported zero errors and zero definitely/indirectly lost bytes on both endpoints.
- Strict warnings with `-Werror` and GCC static analysis completed successfully.
- Portable C++ tests, Python model tests, and the React production build passed.

## Frozen-LSTM integration result

The frozen formal predictions for held-out trace `step-00-s12026` were replayed for windows 204-239
with the validation-selected `0.40` probability gate. The policy committed 13 replicas across five
logical target services.

| Measurement | Observed value |
|---|---:|
| Mean RDMA WRITE completion | 0.945 ms |
| Mean end-to-end replica commit | 6.142 ms |
| Existing policy cost estimate | 2.747 ms |
| Bundle size range | 448-960 bytes |

The result verifies the model-to-policy-to-RDMA path. It is not yet a final performance result. The
current transport creates one CM/QP connection for every copy, so end-to-end time includes setup.
The ten visible entities also produce sub-kilobyte physical bundles, whereas earlier simulations
assumed a 4 MiB logical bucket. Formal claims require persistent connections and matching the real
payload size to the cost model.

## LSTM training decision

The first formal model has already been trained. Its dataset contains 77,760 training samples with
shape `(77760, 6, 5)`, 6,272 severe-hotspot samples, and no skipped non-contiguous windows. CUDA is
available and the frozen checkpoint exists. Therefore the project is ready for model-driven system
testing; repeating the same training is not the next useful step.

Further model changes must use training and validation data only. The existing formal test interval
has already been inspected, so tuning on it again would leak test information. After transport-cost
calibration, generate a new seed range for an untouched final comparison. Real company workload
traces can later replace or supplement synthetic traces if they can be collected ethically and
without confidential data.
