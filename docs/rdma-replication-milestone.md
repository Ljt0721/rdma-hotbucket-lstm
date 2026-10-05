# RDMA bucket-replication milestone

Last updated: 2026-10-05

## What is now implemented

The transport no longer copies a synthetic byte pattern. `EntityMemoryCluster` creates a versioned
bundle from every primary allocation in the selected bucket. The bundle records the bucket and
target node, replica expiry window, entity IDs, source nodes, serialized lengths, aligned lengths,
the actual allocation bytes, and a checksum.

The compute process sends the bundle to a logical memory-node service through a one-sided
`IBV_WR_RDMA_WRITE`. One persistent session per target node keeps its RC QP and 8 MiB local/remote
staging buffers registered for the whole policy run. Every DONE and ACK carries a 64-bit sequence
number. The service checks the sequence, byte length, checksum, bundle structure, bucket ID, and
target node before committing the replica. The simulator updates read routing only after that ACK.

The connection lifecycle follows the repeated-operation pattern in the official
[`rdma-core` rping example](https://github.com/linux-rdma/rdma-core/blob/master/librdmacm/examples/rping.c):
keep the QP established, process multiple work completions, and post the next receive after the
previous receive completes. The project protocol and storage logic are independent implementations.

## Verification performed

- A one-record 576-byte bucket and a two-record 984-byte bucket were transferred and decoded.
- Repeating the same bucket produced generations 1 and 2 while retaining one current stored copy.
- Connecting to the wrong target node produced `RDMA_CM_EVENT_REJECTED` and no replica commit.
- ASan and UBSan reported no memory or undefined-behaviour errors.
- Valgrind reported zero errors and zero definitely/indirectly lost bytes on both endpoints.
- Strict warnings with `-Werror` and GCC static analysis completed successfully.
- Portable C++ tests, Python model tests, and the React production build passed.
- One session committed the same bucket with sequences 1, 2, and 3, then disconnected cleanly.
- A wrong-target session was rejected; the server remained available for a valid client.

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

These values were collected before persistent sessions and remain the comparison baseline.

## Persistent-session result

A single connection copied the same 576-byte bucket three times with sequences 1-3. Connection and
memory-registration setup took 11.236 ms once. Per-copy end-to-end times were 2.727, 2.025, and
1.720 ms; the mean was 2.157 ms. The previous per-copy-connection mean was 6.142 ms. This is a useful
engineering result, not a hardware-RDMA claim, because RXE still executes the RDMA stack in software.

The integrated five-node recent-window run established five sessions once and completed two copies
at 2.858 and 3.511 ms. Each target maintains its own sequence stream. The ten visible entities still
produce sub-kilobyte physical bundles, whereas earlier simulations assumed a 4 MiB logical bucket.
Formal claims require matching payload size and cost-model assumptions and repeating measurements
on physical RNICs.

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
