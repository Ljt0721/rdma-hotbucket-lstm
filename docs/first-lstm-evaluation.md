# First frozen-LSTM system evaluation

Run date: 2026-08-26

## Fixed setup

- 48 synthetic traces across stable, step, ramp, gradual, burst, and random hotspot patterns.
- 240 observation windows per trace; each window represents 30 seconds.
- First 85% of each trace used as action-free history; final 15% used for policy evaluation.
- Five logical memory nodes, ten active entity buckets, and identical GET/PUT requests per policy.
- LSTM probability threshold fixed at 0.70 before system replay.
- CPU inference measured over ten buckets: median 0.424 ms and p95 0.957 ms.
- The conservative p95 inference time is added to every LSTM evaluation window.
- Main copy-cost setting: 4 MiB logical bucket, 1.5 MiB/ms copy bandwidth, 0.08 ms metadata cost.

## Main result

| Policy | Mean completion time | Mean change from no action | Mean copies per trace |
|---|---:|---:|---:|
| No action | 48,249.47 ms | 0.00% | 0.00 |
| Reactive | 15,535.80 ms | 68.50% faster | 8.38 |
| Recent window | 14,881.90 ms | 70.55% faster | 8.81 |
| Cost-aware LSTM | 15,093.58 ms | 69.86% faster | 7.96 |

Paired bootstrap analysis across the 48 traces gives the LSTM a 69.86% mean improvement over no
action, with a 95% interval from 65.58% to 74.06%. Against recent-window prediction, however, the
LSTM is 5.16% slower on average. Its 95% interval is from 1.22% to 9.89% slower, so the current
result does not support a claim that LSTM is the better predictor for this system.

## Copy-cost sensitivity

| Logical bucket size | Reactive | Recent window | Cost-aware LSTM |
|---|---:|---:|---:|
| 4 MiB | 68.50% | 70.55% | 69.86% |
| 16 MiB | 68.22% | 70.25% | 69.60% |
| 64 MiB | 67.12% | 69.12% | 67.47% |

All values are mean completion-time improvements over no action. Higher copy cost reduces benefit,
but it does not reverse the result in this uncalibrated queue model.

## Bottleneck attribution

The LSTM is 211.68 ms slower than recent-window prediction per trace. Replaying the recorded
windows separates this gap as follows:

| Source of the LSTM-minus-recent gap | Mean per trace | Share of gap |
|---|---:|---:|
| Different replica timing and request service | +179.58 ms | 84.84% |
| Conservative p95 LSTM inference charge | +34.44 ms | 16.27% |
| Data-copy cost | -2.35 ms | -1.11% |

The negative copy-cost value means that LSTM copied less data, so copying is not the cause of its
slower result. The main loss comes from replica timing. Recent-window covered 87.32% of severe
bucket-windows, while LSTM covered 85.37%. LSTM made fewer unnecessary copies (9 versus 31), but
it made no copy before the measured load crossed the severe threshold.

The two LSTM outputs explain part of this delay. Its classification output passed the probability
gate on 92.22% of severe bucket-windows. Its GET-count output underestimated severe load by 296
GETs on average, and the combined gates passed on only 84.08%. At the first severe window of each
episode, only 32.79% passed both gates. In 63 onset cases, the classifier indicated a hotspot but
the underestimated GET count blocked the action. The current bottleneck is therefore the
prediction-to-decision design, not model execution or data transfer.

## Interpretation

The model is useful for gradual and ramp workloads, but its extra complexity does not yet improve
the overall system result over the two-window trend. Burst prediction is the weakest model case.
The first LSTM also has two outputs, and a high severe-hotspot probability can disagree with its
predicted GET count. This can delay an otherwise useful copy.

This is a simulation result, not an RDMA performance claim. Node service capacity, queue growth,
copy bandwidth, and metadata time still require calibration. The next model experiment must use
validation data only. After any redesign, a new seed range must be generated as an untouched test
set before the final comparison.
