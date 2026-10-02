#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this script with sudo." >&2
  exit 1
fi

interface=${1:-$(ip route show default | awk 'NR==1 {print $5}')}
if [[ -z ${interface} || ! -d /sys/class/net/${interface} ]]; then
  echo "Network interface not found: ${interface}" >&2
  exit 1
fi

modprobe rdma_rxe
device="rxe_${interface//[^a-zA-Z0-9_]/_}"
if ! rdma link show | grep -q "netdev ${interface}"; then
  rdma link add "${device}" type rxe netdev "${interface}"
fi

echo "Soft-RoCE device over ${interface}:"
rdma link show
ibv_devices
ibv_devinfo -d "${device}" || ibv_devinfo
