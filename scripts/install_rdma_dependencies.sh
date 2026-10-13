#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this script with sudo." >&2
  exit 1
fi

apt-get update
apt-get install -y \
  build-essential \
  cmake \
  rdma-core \
  ibverbs-utils \
  librdmacm-dev \
  libibverbs-dev \
  perftest
