#!/usr/bin/env python3
"""Measure persistent RDMA replica cost across valid bucket-bundle sizes."""

from __future__ import annotations

import argparse
import csv
import json
import queue
import shlex
import statistics
import subprocess
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SIZES = "4096,65536,262144,1048576,4194304"


def parse_sizes(text: str) -> list[int]:
    sizes = [int(item.strip()) for item in text.split(",") if item.strip()]
    if not sizes or any(size <= 0 for size in sizes):
        raise argparse.ArgumentTypeError("sizes must contain positive comma-separated bytes")
    if len(set(sizes)) != len(sizes):
        raise argparse.ArgumentTypeError("sizes must not contain duplicates")
    return sizes


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--memory-host", default="rdma-memory")
    parser.add_argument("--client-host", default="rdma-client")
    parser.add_argument("--memory-ip", default="192.168.64.128")
    parser.add_argument("--remote-root", default="/home/ljt/rdma-hotbucket-lab")
    parser.add_argument("--remote-build", default="build-test")
    parser.add_argument("--port", type=int, default=7690)
    parser.add_argument("--sizes", type=parse_sizes, default=parse_sizes(DEFAULT_SIZES))
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--iterations", type=int, default=30)
    parser.add_argument(
        "--output-dir", type=Path, default=ROOT / "results" / "rdma-calibration"
    )
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    if args.warmup < 0 or args.iterations < 1:
        parser.error("warmup must be non-negative and iterations must be positive")
    return args


def ssh_args(host: str, command: str) -> list[str]:
    return [
        "ssh",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=5",
        "-o",
        "ServerAliveInterval=5",
        "-o",
        "ServerAliveCountMax=2",
        host,
        command,
    ]


def run_remote(host: str, command: str, timeout: int = 30) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ssh_args(host, command),
        check=True,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def parse_key_values(line: str) -> tuple[str, dict[str, str]]:
    parts = line.strip().split()
    values: dict[str, str] = {}
    for part in parts[1:]:
        if "=" in part:
            name, value = part.split("=", 1)
            values[name] = value
    return (parts[0] if parts else ""), values


def percentile(values: Iterable[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("cannot calculate a percentile of no values")
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def anchored_cost_model(
    summaries: list[dict[str, object]], metric: str
) -> dict[str, float | str | None]:
    points = sorted(
        (
            float(row["encoded_bytes"]) / (1024.0 * 1024.0),
            float(row[metric]),
        )
        for row in summaries
    )
    base_x, base_y = points[0]
    if len(points) == 1:
        return {
            "method": "smallest-payload anchor",
            "fixed_cost_ms": base_y,
            "milliseconds_per_mib": None,
            "copy_bandwidth_mib_per_ms": None,
            "effective_mib_per_second": None,
            "r_squared": None,
        }
    denominator = sum((size - base_x) ** 2 for size, _ in points[1:])
    slope = sum(
        (size - base_x) * (latency - base_y) for size, latency in points[1:]
    ) / denominator
    intercept = max(0.0, base_y - slope * base_x)
    mean_y = statistics.fmean(latency for _, latency in points)
    total = sum((latency - mean_y) ** 2 for _, latency in points)
    residual = sum(
        (latency - (intercept + slope * size)) ** 2 for size, latency in points
    )
    r_squared = 1.0 - residual / total if total > 0 else 1.0
    return {
        "method": "smallest-payload anchor",
        "fixed_cost_ms": intercept,
        "milliseconds_per_mib": slope,
        "copy_bandwidth_mib_per_ms": 1.0 / slope if slope > 0 else None,
        "effective_mib_per_second": 1000.0 / slope if slope > 0 else None,
        "r_squared": r_squared,
    }


def wait_for_server(process: subprocess.Popen[str], timeout: float = 20.0) -> list[str]:
    messages: queue.Queue[str | None] = queue.Queue()
    collected: list[str] = []

    def read_output() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            text = line.rstrip()
            collected.append(text)
            messages.put(text)
        messages.put(None)

    threading.Thread(target=read_output, daemon=True).start()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            message = messages.get(timeout=min(0.5, deadline - time.monotonic()))
        except queue.Empty:
            if process.poll() is not None:
                break
            continue
        if message is None:
            break
        if "replica_server_ready" in message:
            return collected
    raise RuntimeError(
        "replica service did not become ready: " + " | ".join(collected[-10:])
    )


def main() -> None:
    args = parse_args()
    binary = f"{args.remote_root}/{args.remote_build}/hotbucket_rdma_replica"
    quoted_binary = shlex.quote(binary)
    health_command = (
        f"test -x {quoted_binary} && "
        "test -n \"$(rdma link show)\" && rdma link show"
    )
    for host in (args.memory_host, args.client_host):
        result = run_remote(host, health_command)
        print(f"{host}: {result.stdout.strip()}")

    stop_command = (
        f"pkill -TERM -f '[h]otbucket_rdma_replica server --port {args.port}' || true"
    )
    run_remote(args.memory_host, stop_command)
    server_command = " ".join(
        [
            "exec",
            quoted_binary,
            "server",
            "--port",
            str(args.port),
            "--node-id",
            "0",
            "--max-replications",
            "0",
            "--max-bundle-bytes",
            str(64 * 1024 * 1024),
        ]
    )
    server = subprocess.Popen(
        ssh_args(args.memory_host, server_command),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    measurements: list[dict[str, object]] = []
    setup_rows: list[dict[str, object]] = []
    try:
        wait_for_server(server)
        print(f"replica service ready on {args.memory_host}:{args.port}")
        for index, payload_bytes in enumerate(args.sizes):
            command = " ".join(
                [
                    quoted_binary,
                    "client",
                    "--server",
                    shlex.quote(args.memory_ip),
                    "--port",
                    str(args.port),
                    "--bucket",
                    str(100 + index),
                    "--target-node",
                    "0",
                    "--payload-bytes",
                    str(payload_bytes),
                    "--warmup",
                    str(args.warmup),
                    "--repeat",
                    str(args.iterations),
                ]
            )
            result = run_remote(args.client_host, command, timeout=180)
            setup_ms: float | None = None
            encoded_bytes: int | None = None
            size_rows: list[dict[str, object]] = []
            for line in result.stdout.splitlines():
                event, values = parse_key_values(line)
                if event == "replica_session_connected":
                    setup_ms = float(values["setup_ms"])
                elif event == "calibration_bundle":
                    encoded_bytes = int(values["encoded_bytes"])
                elif event == "replication_complete":
                    transferred = int(values["bytes"])
                    row: dict[str, object] = {
                        "payload_bytes": payload_bytes,
                        "encoded_bytes": transferred,
                        "iteration": int(values["iteration"]),
                        "sequence": int(values["sequence"]),
                        "write_completion_ms": float(values["write_completion_ms"]),
                        "end_to_end_ms": float(values["end_to_end_ms"]),
                        "provider": values["provider"],
                    }
                    measurements.append(row)
                    size_rows.append(row)
            if setup_ms is None or encoded_bytes is None:
                raise RuntimeError(f"client output is missing calibration metadata:\n{result.stdout}")
            if len(size_rows) != args.iterations:
                raise RuntimeError(
                    f"expected {args.iterations} rows for {payload_bytes} bytes, "
                    f"received {len(size_rows)}"
                )
            setup_rows.append(
                {
                    "payload_bytes": payload_bytes,
                    "encoded_bytes": encoded_bytes,
                    "setup_ms": setup_ms,
                }
            )
            print(
                f"{payload_bytes} payload bytes: "
                f"WRITE p50={percentile((float(row['write_completion_ms']) for row in size_rows), 0.50):.3f} ms, "
                f"ACK p95={percentile((float(row['end_to_end_ms']) for row in size_rows), 0.95):.3f} ms"
            )
    finally:
        try:
            run_remote(args.memory_host, stop_command)
        except (subprocess.SubprocessError, OSError):
            pass
        if server.poll() is None:
            server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=5)

    summaries = []
    for setup in setup_rows:
        payload_bytes = int(setup["payload_bytes"])
        rows = [row for row in measurements if row["payload_bytes"] == payload_bytes]
        write_values = [float(row["write_completion_ms"]) for row in rows]
        end_to_end_values = [float(row["end_to_end_ms"]) for row in rows]
        summaries.append(
            {
                **setup,
                "samples": len(rows),
                "write_mean_ms": statistics.fmean(write_values),
                "write_p50_ms": percentile(write_values, 0.50),
                "write_p95_ms": percentile(write_values, 0.95),
                "end_to_end_mean_ms": statistics.fmean(end_to_end_values),
                "end_to_end_p50_ms": percentile(end_to_end_values, 0.50),
                "end_to_end_p95_ms": percentile(end_to_end_values, 0.95),
            }
        )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    measurements_path = args.output_dir / "measurements.csv"
    with measurements_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(measurements[0]))
        writer.writeheader()
        writer.writerows(measurements)

    cost_models = {
        "p50_end_to_end": anchored_cost_model(summaries, "end_to_end_p50_ms"),
        "p95_end_to_end": anchored_cost_model(summaries, "end_to_end_p95_ms"),
    }
    report = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "environment": {
            "memory_host": args.memory_host,
            "client_host": args.client_host,
            "memory_ip": args.memory_ip,
            "transport": "Soft-RoCE",
            "persistent_session_per_payload_size": True,
        },
        "settings": {
            "sizes": args.sizes,
            "warmup": args.warmup,
            "iterations": args.iterations,
        },
        "summary": summaries,
        "cost_models": cost_models,
    }
    report_path = args.output_dir / "summary.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"measurements: {measurements_path}")
    print(f"summary: {report_path}")
    print(json.dumps(cost_models, indent=2))


if __name__ == "__main__":
    main()
