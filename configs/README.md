# Experiment configuration

The simulator currently uses command-line options so every run is visible in shell history and
easy to reproduce. The default values are defined in `SimulationConfig`.

Example:

```bash
./build/hotbucket_sim --policy no-action --nodes 5 --buckets 64 --entities 10 \
  --windows 40 --requests 4000 --node-capacity 1800 \
  --read-ratio 0.9 --hotspot-share 0.65 --hotspot-duration 5 \
  --replica-ttl 3 --threshold 1200 --bucket-size-mib 4 \
  --copy-bandwidth-mib-per-ms 1.5 --metadata-cost-ms 0.08 \
  --seed 42 --output results/no-action.csv
```

The default numerical latency and copy parameters remain portable placeholders. For the current
two-VM Soft-RoCE setup, `scripts/run_rdma_calibration.py` produces a machine-readable p50/p95 cost
model. Pass its `summary.json` to `scripts/run_formal_evaluation.py --rdma-calibration` rather than
copying rounded values by hand. RXE calibration supports local VM experiments only; physical-RNIC
claims still require a new calibration on that hardware.
