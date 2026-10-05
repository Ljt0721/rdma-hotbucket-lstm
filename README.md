# Cost-Aware LSTM Hot-Bucket Replication

Research prototype for testing whether low-frequency LSTM prediction can improve severe read
hotspots in a small RDMA-style entity-state key-value store after data-copy cost is counted.

The project is intentionally built in stages. The current stage combines a deterministic C++
simulator with an offline PyTorch training pipeline. The simulator creates moving bucket hotspots,
routes GET/PUT requests across memory nodes, applies temporary read replicas, and can export raw
window-level demand without allowing a balancing policy to alter the training trace.

## Current status

- Hash-bucket entity-state workload (`EntityID -> EntityAttribute` access pattern)
- Moving, read-heavy hotspots
- Multiple simulated memory nodes
- Ten battlefield entities serialized into five independent byte-addressed node pools
- Browser view of each entity's node, hash bucket, memory offset, allocation size, and attributes
- Temporary bucket replicas backed by copied entity bytes, TTL reclamation, and GET read splitting
- Write-amplification and configurable copy-cost model
- No-action, reactive, recent-window, and cost-aware LSTM policies
- Reproducible CSV logs and unit tests
- Policy-independent traces with stable, step, ramp, gradual, burst, and random hotspots
- Chronological train/validation/test construction using six windows to predict the next window
- A small shared LSTM with early stopping, checkpoints, metrics, and test prediction export
- Frozen prediction replay with copy cost, measured inference overhead, and paired comparisons
- Versioned bucket bundles containing real entity bytes, allocation boundaries, TTL, and checksum
- RDMA replica services that validate and commit bucket bundles before returning an ACK
- Persistent RDMA sessions with one long-lived QP and registered staging buffer per target node
- Sequence-checked DONE/ACK messages and separate WRITE/end-to-end replication telemetry

The trained LSTM is connected to the replication policy through a prediction CSV. The normal
`hotbucket_sim` keeps the deterministic local copy path. On Linux, `hotbucket_rdma_sim` sends every
accepted copy decision to the selected RDMA replica service and updates routing only after the
remote node validates and commits the bucket. Soft-RoCE is useful for protocol correctness, but
physical-RNIC measurements are still required for hardware performance claims.

The open-source selection and staged implementation plan are documented in
[`docs/open-source-survey.md`](docs/open-source-survey.md). The project deliberately reuses small,
understandable components instead of treating a large paper artifact as the thesis implementation.
The first real-bucket transport result and its current limits are recorded in
[`docs/rdma-replication-milestone.md`](docs/rdma-replication-milestone.md).
After cloning, fetch the read-only references with:

```powershell
git submodule update --init --recursive
```

## Build

### Windows quick start

Build the simulator and tests with the installed MinGW compiler:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_windows.ps1
```

Run one policy directly:

```powershell
.\build\hotbucket_sim.exe --policy recent-window --output results\recent-window.csv
```

Run all current policies against the same generated request trace:

```powershell
python scripts\run_baselines.py --executable .\build\hotbucket_sim.exe
```

### CMake build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

On a single-config MinGW build, run:

```bash
./build/hotbucket_sim --policy no-action --output results/no-action.csv
./build/hotbucket_sim --policy reactive --output results/reactive.csv
./build/hotbucket_sim --policy recent-window --output results/recent-window.csv
```

Use the same seed and workload arguments for every policy. The program prints total simulated
completion time, maximum node load, copy count, and the CSV path.

To run all current baselines with exactly the same configuration:

```bash
python scripts/run_baselines.py --executable ./build/hotbucket_sim
```

## Prepare and train the LSTM

Install the two Python dependencies, generate fixed workload traces, build the time-ordered
dataset, and train the first model:

```powershell
python -m pip install -r ml/requirements.txt
python -m ml.generate_traces --traces-per-pattern 4 --windows 180
python -m ml.build_dataset
python -m ml.train
```

For a short pipeline check, use two traces per pattern, 120 windows, and 3 training epochs:

```powershell
python -m ml.generate_traces --traces-per-pattern 2 --windows 120
python -m ml.build_dataset
python -m ml.train --epochs 3 --device cpu
```

The model sees six recent 30-second measurements for one bucket. Its outputs are the probability
that the bucket will be a severe GET hotspot in the next window and the predicted next-window GET
share. Training artifacts are written to `models/lstm/`; raw and prepared data remain untracked
under `data/`. See [`ml/README.md`](ml/README.md) for the exact fields and split rules.

Export frozen test predictions, measure one ten-bucket CPU inference, and replay all four policies:

```powershell
python -m ml.export_predictions --device cuda
python -m ml.benchmark_inference --device cpu --cpu-threads 1
python scripts/run_formal_evaluation.py --bucket-size-mib 4 `
  --lstm-inference-cost-ms 0.9567
python scripts/analyze_lstm_bottleneck.py
```

Decision gates must be selected on validation windows. The tuning script replays identical
validation traces for each probability and predicted-GET gate pair:

```powershell
python -m ml.export_predictions --split validation `
  --output data/predictions/formal-validation.csv
python scripts/tune_policy_validation.py
```

The first controlled result is recorded in
[`docs/first-lstm-evaluation.md`](docs/first-lstm-evaluation.md). It does not show that LSTM beats
the lightweight predictor; that remains an experimental question rather than a promised outcome.
The validation-only gate correction and fresh-seed confirmation are recorded in
[`docs/prediction-improvement.md`](docs/prediction-improvement.md).

## Live dashboard

The dashboard streams each completed simulator window through a local Server-Sent Events endpoint.
It displays logical node load, hot-bucket movement, GET/PUT activity, copy decisions, and simulated
completion time. It displays the `EntityID -> bucket -> node -> memory offset` placement for ten
battlefield entities and updates the five node pools after every window. Temporary copies occupy
new byte ranges, show their expiry window, and disappear when their TTL ends. It is a view of the
current mathematical simulator, not physical RDMA traffic.

On Windows, the complete start command is:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/start_dashboard.ps1
```

Then open [http://127.0.0.1:5173](http://127.0.0.1:5173). The first run installs the dashboard's
Node packages. To start it manually:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_windows.ps1
cd dashboard
npm install
npm run dev
```

The React development server uses port `5173`; the local simulation API uses port `8787`. Every run
also writes its full CSV data under `results/dashboard/`.

### Debug logs

The Node service writes structured JSONL logs to:

```text
results/dashboard/logs/dashboard-server.jsonl
```

Each run has a UUID `runId`. Log entries record simulator startup, child PID, sanitized run settings,
every completed window, copy decisions, stderr, client disconnects, completion, and exit failures.
The dashboard shows recent events, while the complete backend log is available at:

```text
http://127.0.0.1:8787/api/logs
http://127.0.0.1:8787/api/logs?runId=<run-id>&limit=200
```

## Software RDMA

The optional `hotbucket_rdma_copy` target uses real `rdma_cm` and `libibverbs` calls. It can run on
Linux Soft-RoCE/RXE first and on a physical RNIC later without changing the source-level transport
API. Setup, VM topology, build commands, and limitations are in
[`docs/soft-roce.md`](docs/soft-roce.md).

The replication service maps logical node IDs to consecutive ports. On the memory VM, start five
logical memory-node services at ports 7600-7604:

```bash
bash scripts/start_rdma_replica_nodes.sh 5 7600
```

Then run a policy on the compute VM with real RDMA copies:

```bash
./build-rdma/hotbucket_rdma_sim \
  --policy recent-window --windows 40 \
  --rdma-server 192.168.64.128 --rdma-port-base 7600 \
  --output results/recent-window-rdma.csv
```

The simulator establishes all target-node sessions once before the window loop. The output CSV
distinguishes pure `rdma_write_ms` from steady-state `rdma_end_to_end_ms` and records the
per-session `rdma_sequence`. CM/QP setup is measured by the standalone client, not charged to every
bucket copy.

With the dashboard running, verify the SSE and dynamic-memory path with:

```powershell
cd dashboard
npm run verify:live
```

## Repository layout

```text
include/hotbucket/  Core data types and interfaces
src/                Workload, cluster simulator, policies, and main program
tests/              Dependency-free unit tests
ml/                 Trace generation, dataset preparation, LSTM model, and training
scripts/            Repeatable multi-policy experiment runner
configs/            Reproducible run examples and parameter notes
docs/               Research design and upstream attribution
results/            Generated CSV files (ignored by Git)
third_party/         External research artifacts as Git submodules
```

## Next implementation milestones

1. Send replica-expiry commands to the remote service so physical and simulated lifetimes match.
2. Calibrate copy, metadata, and request service costs on physical RDMA hardware.
3. Create a validation-only model experiment for abrupt hotspots; do not tune against the consumed
   formal test interval.
4. Generate a new untouched test corpus and repeat the primary comparison after calibration.

## Research rule

LSTM will predict future bucket demand, but it will not directly order a copy. A separate
cost-aware rule will compare expected benefit with copy cost and wrong-prediction risk. A negative
result is valid: if LSTM does not reduce end-to-end completion time, the thesis will report that a
simpler policy is more suitable under the tested conditions.
