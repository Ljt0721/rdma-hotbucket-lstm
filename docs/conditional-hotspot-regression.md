# Conditional hotspot-load experiment

Run date: 2026-10-13

## Why the model changed

The first LSTM had two outputs: severe-hotspot probability and unconditional next-window GET
share. The controller multiplied the predicted copy benefit by the hotspot probability. This
discounted the expected load twice because the unconditional regression output already included
ordinary windows. After the measured Soft-RoCE copy cost was introduced, changing the probability
gate from 0.10 to 0.90 barely changed the decision count. The controller was rejecting actions in
the utility calculation rather than at the probability gate.

The revised training option keeps the same 5,058-parameter LSTM and classifier. Its regression loss
is evaluated only on severe-hotspot samples, so the two outputs now mean:

- probability that the bucket is severe in the next 30-second window;
- expected GET share conditional on that severe hotspot occurring.

The controller can therefore use `probability x conditional benefit - measured copy cost` without
the previous double discount.

## Leak-free selection

The architecture was trained on the original chronological training interval. The probability
gate was selected only on the 70%-85% validation interval using the measured p50 Soft-RoCE model:
`2.337237 ms + size / 0.012051 MiB/ms`. A narrow validation search selected probability 0.50. The
predicted-GET hard gate remained disabled because the copy-utility rule already checks benefit.

On validation traces, the old LSTM was 10.76% slower than recent-window prediction after RDMA
calibration. The conditional model was 0.69% faster at the selected gate. Model and gate were then
frozen before generating confirmation traces.

## Fresh-seed confirmation

The confirmation corpus contains 48 new traces from seed range 42026-47033, with eight traces for
each of six workload patterns. The simulator replayed no action, reactive copying, recent-window
prediction, and the frozen conditional LSTM on identical requests.

| Comparison | Mean relative result | Bootstrap 95% interval | Wins |
|---|---:|---:|---:|
| LSTM vs no action | +56.65% | +50.37% to +62.58% | 47/48 |
| LSTM vs reactive | -1.27% | -5.38% to +2.74% | 14/48 |
| LSTM vs recent window | +0.20% | -3.74% to +4.03% | 16/48 |

The revised LSTM covered 83.05% of severe bucket-windows, compared with 80.46% for recent-window
prediction. Its severe-load bias improved from about -297 GETs in the previous model to -12 GETs.
However, it made 427 copies rather than 390 and its useful-copy rate was 85.95% rather than 92.31%.
The extra coverage saved request service time, but the extra copies consumed most of that gain.

## Claim boundary

The result supports two limited conclusions. First, the old loss/controller semantics were a real
performance defect, and correcting them removed an approximately 11% disadvantage under calibrated
copy cost. Second, the revised LSTM is competitive with the recent-window baseline on fresh traces.
The confidence interval still includes zero, so the experiment does not show that LSTM consistently
beats the simpler predictor. Burst and random hotspot onsets remain difficult because some onsets
have no warning in the six observed windows.

The dashboard was also checked end to end with the frozen LSTM and five Soft-RoCE memory services.
One 12-window run triggered three validated RDMA copies. This confirms protocol integration, not
physical-RNIC performance; RXE still executes the RDMA verbs in software.
