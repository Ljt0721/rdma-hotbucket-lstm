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

The trained LSTM is connected to the simulated replication policy through a prediction CSV. Real
RDMA CM/verbs transport now has a Linux copy probe and a documented Soft-RoCE path. It is not yet
wired into every simulator copy. The current simulator does not claim to reproduce physical RDMA
latency until its parameters are calibrated.

The open-source selection and staged implementation plan are documented in
[`docs/open-source-survey.md`](docs/open-source-survey.md). The project deliberately reuses small,
understandable components instead of treating a large paper artifact as the thesis implementation.
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

1. Create a new validation-only model experiment for the cases where LSTM misses abrupt changes;
   do not tune against the consumed formal test interval.
2. Split the KV core from its transport, then add local and socket transports for multi-process
   testing.
3. Add CM/verbs transport and calibrate copy, metadata, and request service costs with Soft-RoCE.
4. Generate a new untouched test corpus and repeat the primary comparison after calibration.

## Research rule

LSTM will predict future bucket demand, but it will not directly order a copy. A separate
cost-aware rule will compare expected benefit with copy cost and wrong-prediction risk. A negative
result is valid: if LSTM does not reduce end-to-end completion time, the thesis will report that a
simpler policy is more suitable under the tested conditions.
