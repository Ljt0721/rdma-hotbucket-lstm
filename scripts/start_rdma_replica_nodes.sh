#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
node_count="${1:-5}"
base_port="${2:-7600}"
maximum_replications="${3:-0}"
binary="${repo_root}/build-rdma/hotbucket_rdma_replica"
log_dir="${repo_root}/results/rdma"

if [[ ! -x "${binary}" ]]; then
  echo "Missing ${binary}; build with -DHOTBUCKET_ENABLE_RDMA=ON first." >&2
  exit 1
fi
if ! [[ "${node_count}" =~ ^[1-9][0-9]*$ && "${base_port}" =~ ^[0-9]+$ &&
        "${maximum_replications}" =~ ^[0-9]+$ ]]; then
  echo "Usage: $0 [node-count] [base-port] [max-replications-per-node]" >&2
  exit 1
fi
if (( base_port < 1 || base_port + node_count - 1 > 65535 )); then
  echo "The requested node ports are outside 1-65535." >&2
  exit 1
fi

mkdir -p "${log_dir}"
pids=()
cleanup() {
  for pid in "${pids[@]:-}"; do
    kill "${pid}" 2>/dev/null || true
  done
}
trap cleanup EXIT INT TERM

for ((node = 0; node < node_count; ++node)); do
  port=$((base_port + node))
  log_path="${log_dir}/node-${node}.log"
  "${binary}" server \
    --port "${port}" \
    --node-id "${node}" \
    --max-replications "${maximum_replications}" \
    >"${log_path}" 2>&1 &
  pids+=("$!")
  echo "node=${node} port=${port} pid=${pids[-1]} log=${log_path}"
done

wait
