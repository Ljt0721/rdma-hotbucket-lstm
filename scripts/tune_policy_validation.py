#!/usr/bin/env python3
"""Select LSTM decision gates on validation windows only."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import subprocess
from pathlib import Path
from typing import Dict, List


def parse_summary(stdout: str) -> Dict[str, str]:
    summary = {}
    for line in stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            summary[key.strip()] = value.strip()
    return summary


def load_manifest(path: Path) -> List[Dict[str, str]]:
    with path.open("r", newline="", encoding="utf-8") as source:
        return list(csv.DictReader(source))


def relative_to_root(path: Path, root: Path) -> Path:
    try:
        return path.resolve().relative_to(root.resolve())
    except ValueError:
        return path.resolve()


def run_policy(
    root: Path,
    executable: Path,
    prediction_file: Path,
    trace: Dict[str, str],
    policy: str,
    output: Path,
    evaluation_start_fraction: float,
    evaluation_end_fraction: float,
    probability: float,
    predicted_get_ratio: float,
    inference_cost_ms: float,
    bucket_size_mib: float,
) -> Dict[str, float]:
    source_windows = int(trace["windows"])
    windows = int(source_windows * evaluation_end_fraction)
    evaluation_start = int(source_windows * evaluation_start_fraction)
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        str(relative_to_root(executable, root)),
        "--policy", policy,
        "--trace-id", trace["trace_id"],
        "--workload-pattern", trace["pattern"],
        "--nodes", "5",
        "--buckets", "64",
        "--entities", "10",
        "--windows", str(windows),
        "--requests", trace["requests_per_window"],
        "--node-capacity", trace["node_capacity"],
        "--hotspot-duration", trace["hotspot_duration"],
        "--read-ratio", trace["read_ratio"],
        "--hotspot-share", trace["hotspot_share"],
        "--threshold", trace["severe_get_threshold"],
        "--seed", trace["seed"],
        "--window-seconds", trace["window_seconds"],
        "--evaluation-start-window", str(evaluation_start),
        "--replica-ttl", "3",
        "--bucket-size-mib", str(bucket_size_mib),
        "--copy-bandwidth-mib-per-ms", "1.5",
        "--metadata-cost-ms", "0.08",
        "--output", str(relative_to_root(output, root)),
    ]
    if policy == "lstm":
        command.extend(
            [
                "--prediction-file", str(relative_to_root(prediction_file, root)),
                "--lstm-min-probability", str(probability),
                "--lstm-min-predicted-get-ratio", str(predicted_get_ratio),
                "--lstm-inference-cost-ms", str(inference_cost_ms),
            ]
        )
    try:
        completed = subprocess.run(
            command,
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
    except subprocess.CalledProcessError as error:
        raise RuntimeError(
            "{} failed for {}:\n{}\n{}".format(
                policy, trace["trace_id"], error.stdout, error.stderr
            )
        ) from error
    summary = parse_summary(completed.stdout)
    return {
        "completion_ms": float(summary["total_completion_ms"]),
        "copies": float(summary["copies"]),
    }


def tune(args: argparse.Namespace) -> Dict[str, object]:
    root = Path(__file__).resolve().parents[1]
    traces = load_manifest(args.manifest.resolve())
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    baselines: Dict[str, Dict[str, Dict[str, float]]] = {}
    for trace in traces:
        trace_id = trace["trace_id"]
        baselines[trace_id] = {}
        for policy in ("no-action", "recent-window"):
            baselines[trace_id][policy] = run_policy(
                root,
                args.executable.resolve(),
                args.prediction_file.resolve(),
                trace,
                policy,
                output_dir / "baselines" / trace_id / "{}.csv".format(policy),
                args.evaluation_start_fraction,
                args.evaluation_end_fraction,
                0.0,
                0.0,
                0.0,
                args.bucket_size_mib,
            )

    trials: List[Dict[str, object]] = []
    for probability in args.probabilities:
        for predicted_get_ratio in args.predicted_get_ratios:
            completion = []
            copies = []
            improvements_no_action = []
            improvements_recent = []
            trial_name = "p{:02d}-g{:03d}".format(
                round(probability * 100), round(predicted_get_ratio * 100)
            )
            for trace in traces:
                trace_id = trace["trace_id"]
                result = run_policy(
                    root,
                    args.executable.resolve(),
                    args.prediction_file.resolve(),
                    trace,
                    "lstm",
                    output_dir / "runs" / trial_name / "{}.csv".format(trace_id),
                    args.evaluation_start_fraction,
                    args.evaluation_end_fraction,
                    probability,
                    predicted_get_ratio,
                    args.inference_cost_ms,
                    args.bucket_size_mib,
                )
                no_action = baselines[trace_id]["no-action"]["completion_ms"]
                recent = baselines[trace_id]["recent-window"]["completion_ms"]
                completion.append(result["completion_ms"])
                copies.append(result["copies"])
                improvements_no_action.append(
                    (no_action - result["completion_ms"]) / no_action * 100.0
                )
                improvements_recent.append(
                    (recent - result["completion_ms"]) / recent * 100.0
                )
            row: Dict[str, object] = {
                "minimum_probability": probability,
                "minimum_predicted_get_ratio": predicted_get_ratio,
                "mean_completion_ms": statistics.fmean(completion),
                "mean_improvement_vs_no_action_pct": statistics.fmean(
                    improvements_no_action
                ),
                "mean_improvement_vs_recent_pct": statistics.fmean(improvements_recent),
                "mean_copies": statistics.fmean(copies),
                "wins_vs_recent": sum(value > 0.0 for value in improvements_recent),
                "traces": len(traces),
            }
            trials.append(row)
            print(json.dumps(row))

    trials.sort(key=lambda row: float(row["mean_completion_ms"]))
    fields = tuple(trials[0].keys())
    with (output_dir / "trials.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(trials)

    report = {
        "selection_data": "validation windows only",
        "evaluation_start_fraction": args.evaluation_start_fraction,
        "evaluation_end_fraction": args.evaluation_end_fraction,
        "best": trials[0],
        "trials": trials,
    }
    with (output_dir / "best.json").open("w", encoding="utf-8") as output:
        json.dump(report, output, indent=2)
    print("best={}".format(json.dumps(trials[0])))
    return report


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, default=root / "build" / "hotbucket_sim.exe")
    parser.add_argument(
        "--manifest",
        type=Path,
        default=root / "data" / "traces" / "formal" / "raw" / "manifest.csv",
    )
    parser.add_argument(
        "--prediction-file",
        type=Path,
        default=root / "data" / "predictions" / "formal-validation.csv",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=root / "results" / "validation-policy-tuning",
    )
    parser.add_argument("--probabilities", nargs="+", type=float, default=(0.50, 0.60, 0.70))
    parser.add_argument(
        "--predicted-get-ratios", nargs="+", type=float, default=(0.0, 0.85, 1.0)
    )
    parser.add_argument("--evaluation-start-fraction", type=float, default=0.70)
    parser.add_argument("--evaluation-end-fraction", type=float, default=0.85)
    parser.add_argument("--inference-cost-ms", type=float, default=0.9567)
    parser.add_argument("--bucket-size-mib", type=float, default=4.0)
    return parser.parse_args()


if __name__ == "__main__":
    tune(parse_args())
