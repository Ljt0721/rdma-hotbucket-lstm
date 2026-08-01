#!/usr/bin/env python3
"""Run every baseline with one shared workload configuration and seed."""

from __future__ import annotations

import argparse
import csv
import subprocess
from pathlib import Path


POLICIES = ("no-action", "reactive", "recent-window")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--results-dir", type=Path, default=Path("results"))
    parser.add_argument("--nodes", type=int, default=4)
    parser.add_argument("--buckets", type=int, default=128)
    parser.add_argument("--windows", type=int, default=40)
    parser.add_argument("--requests", type=int, default=4000)
    parser.add_argument("--node-capacity", type=int, default=1800)
    parser.add_argument("--hotspot-duration", type=int, default=5)
    parser.add_argument("--replica-ttl", type=int, default=3)
    parser.add_argument("--threshold", type=int, default=1200)
    parser.add_argument("--seed", type=int, default=42)
    return parser.parse_args()


def parse_summary(stdout: str) -> dict[str, str]:
    summary: dict[str, str] = {}
    for line in stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            summary[key] = value
    return summary


def main() -> None:
    args = parse_args()
    executable = args.executable.resolve()
    if not executable.exists():
        raise SystemExit(f"simulator executable does not exist: {executable}")

    args.results_dir.mkdir(parents=True, exist_ok=True)
    summaries: list[dict[str, str]] = []
    for policy in POLICIES:
        output = args.results_dir / f"{policy}-seed-{args.seed}.csv"
        command = [
            str(executable),
            "--policy", policy,
            "--nodes", str(args.nodes),
            "--buckets", str(args.buckets),
            "--windows", str(args.windows),
            "--requests", str(args.requests),
            "--node-capacity", str(args.node_capacity),
            "--hotspot-duration", str(args.hotspot_duration),
            "--replica-ttl", str(args.replica_ttl),
            "--threshold", str(args.threshold),
            "--seed", str(args.seed),
            "--output", str(output),
        ]
        completed = subprocess.run(command, check=True, text=True, capture_output=True)
        print(completed.stdout, end="")
        summaries.append(parse_summary(completed.stdout))

    summary_path = args.results_dir / f"summary-seed-{args.seed}.csv"
    fields = ("policy", "windows", "total_completion_ms", "max_node_load_ratio", "copies", "csv")
    with summary_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summaries)
    print(f"summary={summary_path}")


if __name__ == "__main__":
    main()
