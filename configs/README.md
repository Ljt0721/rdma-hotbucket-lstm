# Experiment configuration

The simulator currently uses command-line options so every run is visible in shell history and
easy to reproduce. The default values are defined in `SimulationConfig`.

Example:

```bash
./build/hotbucket_sim --policy no-action --nodes 4 --buckets 128 \
  --windows 40 --requests 4000 --node-capacity 1800 \
  --read-ratio 0.9 --hotspot-share 0.65 --hotspot-duration 5 \
  --replica-ttl 3 --threshold 1200 --bucket-size-mib 4 \
  --copy-bandwidth-mib-per-ms 1.5 --metadata-cost-ms 0.08 \
  --seed 42 --output results/no-action.csv
```

The numerical latency and copy parameters are placeholders for the simulation layer. They must be
calibrated with local TCP/VM measurements and, later, RDMA microbenchmarks before physical-system
claims are made.
