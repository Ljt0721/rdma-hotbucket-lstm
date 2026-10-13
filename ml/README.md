# LSTM training pipeline

This directory turns the simulator's fixed GET/PUT demand into a trainable hot-bucket model. It is
offline: training and inference are not placed in the per-request RDMA path.

## What one sample means

The simulator records one row for each active bucket in every 30-second observation window. The
model input is six consecutive rows for the same bucket, or three minutes of recent history. Each
row contains five measured values:

1. GET count;
2. PUT count;
3. GET ratio within that bucket;
4. the bucket's share of all requests;
5. logical load of the bucket's home node.

The answer used during training is taken from the next window. The model learns two outputs:

- whether the future GET count crosses the severe-hotspot threshold;
- what share of all requests will be GETs for that bucket.

The first output helps reject weak events. The second gives the later cost-aware policy a demand
estimate. Neither output directly commands replication.

## Files

- `generate_traces.py`: runs the C++ simulator in `--trace-only` mode for six workload patterns.
- `dataset.py`: creates rolling sequences, future labels, normalization, and saved NumPy splits.
- `build_dataset.py`: command-line entry point for dataset construction.
- `model.py`: one-layer, 32-hidden-unit shared LSTM with classification and regression heads.
- `train.py`: Adam training, class weighting, gradient clipping, validation early stopping,
  checkpointing, terminal/file logging, and held-out prediction export.
- `export_predictions.py`: loads the frozen checkpoint and writes only the fields needed by C++.
- `benchmark_inference.py`: measures one controller pass over ten bucket sequences.

## Data separation

For each trace, target windows are divided in their original order: first 70% for training, next
15% for validation, and final 15% for testing. Context windows may look backward across a boundary,
but their target always belongs to exactly one split. Mean and standard deviation are calculated
from training inputs only. Batches are shuffled only after this temporal separation.

Every policy will later replay the same test requests. A policy may change the serving node, but it
cannot rewrite the original request sequence used as model input.

## Commands

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_windows.ps1
python -m pip install -r ml/requirements.txt
python -m ml.generate_traces --traces-per-pattern 4 --windows 180
python -m ml.build_dataset
python -m ml.train
```

For the larger formal simulation corpus used during development:

```powershell
python -m ml.generate_traces --output-dir data/traces/formal/raw `
  --traces-per-pattern 8 --windows 240 --base-seed 12026
python -m ml.build_dataset --raw-dir data/traces/formal/raw `
  --output-dir data/training/formal
python -m ml.train --dataset-dir data/training/formal `
  --output-dir models/lstm/formal --epochs 60 --patience 8 `
  --batch-size 256 --device cuda
```

The readable log can be followed from another PowerShell terminal while training:

```powershell
Get-Content models/lstm/formal/training.log -Wait
```

`events.jsonl` records the same run as structured events. It includes the environment and command
settings, every epoch, every best-checkpoint update, early stopping, and the final result.

Generated files are intentionally ignored by Git:

```text
data/traces/raw/*.csv       policy-independent simulator rows and manifest
data/training/*.npz         normalized train, validation, and test arrays
data/training/metadata.json feature statistics and sample counts
models/lstm/hotbucket_lstm.pt
models/lstm/metrics.json
models/lstm/training_history.csv
models/lstm/training.log
models/lstm/events.jsonl
models/lstm/test_predictions.csv
```

Classification precision, recall, and F1 are model checks, not the final thesis claim. The final
claim depends on whether replaying fixed test requests with prediction and copying reduces total
completion time after copy and metadata costs are included.

The current formal test interval has already been evaluated. Further model changes must use
training and validation data only, followed by a new untouched final test corpus. Repeatedly tuning
against the existing formal test results would make the reported comparison optimistic.
