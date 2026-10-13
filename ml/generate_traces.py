"""Generate policy-independent bucket workload traces with the C++ simulator."""

from __future__ import annotations

import argparse
import csv
import subprocess
from pathlib import Path
from typing import Dict, Iterable, List


PATTERNS = ("step", "ramp", "gradual", "burst", "random", "stable")


def generate_trace_corpus(
    executable: Path,
    output_dir: Path,
    patterns: Iterable[str],
    traces_per_pattern: int,
    windows: int,
    base_seed: int,
    severe_get_threshold: int,
) -> List[Dict[str, object]]:
    """Run the trace-only simulator and return the generated manifest rows."""
    if traces_per_pattern < 1:
        raise ValueError("traces_per_pattern must be positive")
    if windows < 20:
        raise ValueError("windows must be at least 20 for temporal splitting")
    if not executable.exists():
        raise FileNotFoundError(
            "simulator executable not found: {}. Build it before generating traces.".format(
                executable
            )
        )

    selected_patterns = list(patterns)
    unknown = sorted(set(selected_patterns) - set(PATTERNS))
    if unknown:
        raise ValueError("unknown workload patterns: {}".format(", ".join(unknown)))

    output_dir.mkdir(parents=True, exist_ok=True)
    repository_root = Path(__file__).resolve().parents[1]
    read_ratios = (0.70, 0.80, 0.90)
    hotspot_shares = (0.55, 0.65, 0.75)
    hotspot_durations = (4, 6, 8, 12)
    request_counts = (3500, 4000, 4500, 5000)
    node_capacities = (1600, 1800, 2000)
    manifest: List[Dict[str, object]] = []

    for pattern_index, pattern in enumerate(selected_patterns):
        for trace_index in range(traces_per_pattern):
            seed = base_seed + pattern_index * 1000 + trace_index
            read_ratio = read_ratios[(pattern_index + trace_index) % len(read_ratios)]
            hotspot_share = hotspot_shares[(2 * pattern_index + trace_index) % len(hotspot_shares)]
            hotspot_duration = hotspot_durations[
                (pattern_index + 2 * trace_index) % len(hotspot_durations)
            ]
            requests = request_counts[(pattern_index + trace_index) % len(request_counts)]
            node_capacity = node_capacities[(pattern_index + trace_index) % len(node_capacities)]
            trace_id = "{}-{:02d}-s{}".format(pattern, trace_index, seed)
            trace_path = output_dir / "{}.csv".format(trace_id)
            try:
                command_executable = executable.relative_to(repository_root)
            except ValueError:
                command_executable = executable
            try:
                command_output = trace_path.relative_to(repository_root)
            except ValueError:
                command_output = trace_path

            command = [
                str(command_executable),
                "--trace-only",
                "--trace-id",
                trace_id,
                "--workload-pattern",
                pattern,
                "--nodes",
                "5",
                "--buckets",
                "64",
                "--entities",
                "10",
                "--windows",
                str(windows),
                "--requests",
                str(requests),
                "--node-capacity",
                str(node_capacity),
                "--hotspot-duration",
                str(hotspot_duration),
                "--read-ratio",
                str(read_ratio),
                "--hotspot-share",
                str(hotspot_share),
                "--threshold",
                str(severe_get_threshold),
                "--window-seconds",
                "30",
                "--seed",
                str(seed),
                "--output",
                str(command_output),
            ]
            try:
                completed = subprocess.run(
                    command,
                    check=True,
                    capture_output=True,
                    text=True,
                    cwd=repository_root,
                )
            except subprocess.CalledProcessError as error:
                raise RuntimeError(
                    "trace generation failed for {}:\n{}\n{}".format(
                        trace_id, error.stdout.strip(), error.stderr.strip()
                    )
                ) from error
            print("generated {:<28} {}".format(trace_id, trace_path))
            if completed.stderr.strip():
                print(completed.stderr.strip())

            manifest.append(
                {
                    "trace_id": trace_id,
                    "pattern": pattern,
                    "seed": seed,
                    "windows": windows,
                    "window_seconds": 30,
                    "read_ratio": read_ratio,
                    "hotspot_share": hotspot_share,
                    "hotspot_duration": hotspot_duration,
                    "requests_per_window": requests,
                    "node_capacity": node_capacity,
                    "severe_get_threshold": severe_get_threshold,
                    "path": trace_path.name,
                }
            )

    manifest_path = output_dir / "manifest.csv"
    with manifest_path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=list(manifest[0].keys()))
        writer.writeheader()
        writer.writerows(manifest)
    print("manifest {} ({} traces)".format(manifest_path, len(manifest)))
    return manifest


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--executable",
        type=Path,
        default=root / "build" / "hotbucket_sim.exe",
        help="path to the built C++ simulator",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=root / "data" / "traces" / "raw",
    )
    parser.add_argument("--patterns", nargs="+", choices=PATTERNS, default=list(PATTERNS))
    parser.add_argument("--traces-per-pattern", type=int, default=4)
    parser.add_argument("--windows", type=int, default=180)
    parser.add_argument("--base-seed", type=int, default=2026)
    parser.add_argument("--severe-get-threshold", type=int, default=1200)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    generate_trace_corpus(
        executable=args.executable.resolve(),
        output_dir=args.output_dir.resolve(),
        patterns=args.patterns,
        traces_per_pattern=args.traces_per_pattern,
        windows=args.windows,
        base_seed=args.base_seed,
        severe_get_threshold=args.severe_get_threshold,
    )


if __name__ == "__main__":
    main()
