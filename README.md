# Cost-Aware LSTM Hot-Bucket Replication

Research prototype for testing whether low-frequency LSTM prediction can improve severe read
hotspots in a small RDMA-style entity-state key-value store after data-copy cost is counted.

The project is intentionally built in stages. The current stage is a deterministic C++ simulator
that creates moving bucket hotspots, routes GET/PUT requests across memory nodes, applies temporary
read replicas, and writes window-level measurements for later model training.

## Current status

- Hash-bucket entity-state workload (`EntityID -> EntityAttribute` access pattern)
- Moving, read-heavy hotspots
- Multiple simulated memory nodes
- Temporary bucket replicas and GET read splitting
- Write-amplification and configurable copy-cost model
- No-action, reactive, and cost-aware recent-window baselines
- Reproducible CSV logs and unit tests

The LSTM predictor and real RDMA CM/verbs transport are the next research stages. The current
simulator does not claim to reproduce physical RDMA latency until its parameters are calibrated.

The open-source selection and staged implementation plan are documented in
[`docs/open-source-survey.md`](docs/open-source-survey.md). The project deliberately reuses small,
understandable components instead of treating a large paper artifact as the thesis implementation.

## Build

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

## Repository layout

```text
include/hotbucket/  Core data types and interfaces
src/                Workload, cluster simulator, policies, and main program
tests/              Dependency-free unit tests
scripts/            Repeatable multi-policy experiment runner
configs/            Reproducible run examples and parameter notes
docs/               Research design and upstream attribution
results/            Generated CSV files (ignored by Git)
third_party/         External research artifacts as Git submodules
```

## Next implementation milestones

1. Export time-ordered bucket sequences and train a small PyTorch LSTM offline.
2. Replay frozen predictions through the same cost-aware decision used by the lightweight baseline.
3. Split the KV core from its transport, then add local, socket, and CM/verbs transports in order.
4. Calibrate copy and metadata costs with Soft-RoCE first and RDMA hardware when available.

## Research rule

LSTM will predict future bucket demand, but it will not directly order a copy. A separate
cost-aware rule will compare expected benefit with copy cost and wrong-prediction risk. A negative
result is valid: if LSTM does not reduce end-to-end completion time, the thesis will report that a
simpler policy is more suitable under the tested conditions.
