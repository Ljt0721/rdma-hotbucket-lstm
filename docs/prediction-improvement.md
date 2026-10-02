# Hotspot-coverage improvement experiment

Run date: 2026-09-26

## Problem found

The first controller required both hotspot probability `>= 0.70` and predicted GET count `>= 1200`.
The classifier often recognized a new hotspot while the regression head slightly underestimated its
GET count. This double gate reduced severe-window coverage and delayed replication.

## Validation-only selection

The original training split and checkpoint were kept fixed. Predictions for the chronological
validation interval (70%-85% of each trace) were exported separately. Nine initial combinations and
then a finer probability sweep were replayed through the complete simulator. No test interval was
used to select the gates.

The best validation setting was:

- minimum hotspot probability: `0.40`;
- minimum predicted-GET ratio: `0.0`, which removes the redundant hard GET gate;
- final action still requires positive confidence-adjusted benefit after copy cost.

Its mean validation completion time was `14,635.69 ms`, with a 2.08% mean per-trace improvement
over recent-window prediction. Lowering probability to `0.30` increased unnecessary copies and was
slower.

## Model-loss experiment

A second 32-unit LSTM assigned twice the regression weight to severe hotspots and four times the
weight to severe underprediction. Its validation recall increased to `0.940`, but precision fell to
`0.791`, mean copies rose from `8.83` to `9.33`, and completion time increased from `14,697.53 ms`
to `14,709.79 ms` under the same `0.50` gate. The candidate was therefore rejected. This result
shows why model metrics alone cannot select the system policy.

## Fresh-seed confirmation

After the `0.40/0.0` setting was frozen, 48 new traces were generated from seed range 32026-37033.
The LSTM had not been trained or tuned on these traces.

| Policy comparison | Mean relative result | Bootstrap 95% interval | Wins |
|---|---:|---:|---:|
| LSTM vs no action | +71.20% | +66.68% to +75.67% | 48/48 |
| LSTM vs reactive | +10.15% | +6.45% to +14.18% | 28/48 |
| LSTM vs recent window | +2.20% | -0.32% to +5.00% | 19/48 |

The tuned controller covered 88.78% of severe bucket-windows, compared with 86.98% for
recent-window prediction and 85.37% for the original LSTM controller. Its hotspot-onset gate recall
rose from 32.8% to 65.75%. The useful-copy rate fell to 87.76%, so additional coverage came with
more false positives.

The result fixes the clear double-gate defect and reverses the previous mean loss. It does not yet
prove that LSTM consistently beats the simple predictor because the LSTM-versus-recent confidence
interval still includes zero. Burst workloads remain the main weak case.
