#!/usr/bin/env python3
"""Replay formal held-out windows under all four replication policies."""

from __future__ import annotations

import argparse
import csv
import json
import logging
import random
import statistics
import subprocess
import time
from collections import defaultdict
from pathlib import Path
from typing import DefaultDict, Dict, Iterable, List, Tuple


POLICIES = ("no-action", "reactive", "recent-window", "lstm")


def relative_to_root(path: Path, root: Path) -> Path:
    try:
        return path.resolve().relative_to(root.resolve())
    except ValueError:
        return path.resolve()


def parse_summary(stdout: str) -> Dict[str, str]:
    summary: Dict[str, str] = {}
    for line in stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            summary[key.strip()] = value.strip()
    return summary


def configure_logging(output_dir: Path) -> Tuple[logging.Logger, Path]:
    logger = logging.getLogger("hotbucket.evaluation")
    logger.setLevel(logging.INFO)
    logger.handlers.clear()
    logger.propagate = False
    console = logging.StreamHandler()
    console.setFormatter(logging.Formatter("%(message)s"))
    file_handler = logging.FileHandler(
        output_dir / "evaluation.log", mode="w", encoding="utf-8"
    )
    file_handler.setFormatter(
        logging.Formatter("%(asctime)s %(levelname)s %(message)s", "%Y-%m-%d %H:%M:%S")
    )
    logger.addHandler(console)
    logger.addHandler(file_handler)
    event_path = output_dir / "events.jsonl"
    event_path.unlink(missing_ok=True)
    return logger, event_path


def write_event(path: Path, event_type: str, **fields: object) -> None:
    with path.open("a", encoding="utf-8") as output:
        output.write(
            json.dumps(
                {
                    "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                    "event": event_type,
                    **fields,
                }
            )
            + "\n"
        )


def load_manifest(path: Path, patterns: Iterable[str], max_traces: int) -> List[Dict[str, str]]:
    selected_patterns = set(patterns)
    with path.open("r", newline="", encoding="utf-8") as source:
        rows = [
            row
            for row in csv.DictReader(source)
            if not selected_patterns or row["pattern"] in selected_patterns
        ]
    if max_traces > 0:
        rows = rows[:max_traces]
    if not rows:
        raise ValueError("no manifest traces matched the requested filters")
    return rows


def write_csv(path: Path, rows: List[Dict[str, object]], fields: Tuple[str, ...]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def aggregate_results(results: List[Dict[str, object]]) -> List[Dict[str, object]]:
    groups: DefaultDict[Tuple[str, str], List[Dict[str, object]]] = defaultdict(list)
    for result in results:
        groups[("all", str(result["policy"]))].append(result)
        groups[(str(result["pattern"]), str(result["policy"]))].append(result)

    aggregates: List[Dict[str, object]] = []
    for (pattern, policy), rows in sorted(groups.items()):
        completion = [float(row["total_completion_ms"]) for row in rows]
        improvement = [float(row["improvement_vs_no_action_pct"]) for row in rows]
        copies = [float(row["copies"]) for row in rows]
        aggregates.append(
            {
                "pattern": pattern,
                "policy": policy,
                "runs": len(rows),
                "mean_completion_ms": statistics.fmean(completion),
                "median_completion_ms": statistics.median(completion),
                "mean_improvement_vs_no_action_pct": statistics.fmean(improvement),
                "median_improvement_vs_no_action_pct": statistics.median(improvement),
                "mean_copies": statistics.fmean(copies),
                "wins_vs_no_action": sum(value > 0.0 for value in improvement),
            }
        )
    return aggregates


def bootstrap_mean_interval(
    values: List[float], resamples: int = 5000, seed: int = 2026
) -> Tuple[float, float]:
    generator = random.Random(seed)
    sample_count = len(values)
    means = []
    for _ in range(resamples):
        means.append(
            statistics.fmean(values[generator.randrange(sample_count)] for _ in values)
        )
    means.sort()
    return means[int(resamples * 0.025)], means[int(resamples * 0.975)]


def pairwise_results(results: List[Dict[str, object]]) -> List[Dict[str, object]]:
    traces: DefaultDict[str, Dict[str, Dict[str, object]]] = defaultdict(dict)
    for result in results:
        traces[str(result["trace_id"])][str(result["policy"])] = result
    comparisons = (
        ("reactive", "no-action"),
        ("recent-window", "no-action"),
        ("lstm", "no-action"),
        ("lstm", "reactive"),
        ("lstm", "recent-window"),
    )
    output: List[Dict[str, object]] = []
    for policy, comparator in comparisons:
        differences = []
        for policies in traces.values():
            policy_time = float(policies[policy]["total_completion_ms"])
            comparator_time = float(policies[comparator]["total_completion_ms"])
            differences.append((comparator_time - policy_time) / comparator_time * 100.0)
        lower, upper = bootstrap_mean_interval(differences)
        tolerance = 1e-9
        output.append(
            {
                "policy": policy,
                "comparator": comparator,
                "traces": len(differences),
                "mean_relative_improvement_pct": statistics.fmean(differences),
                "median_relative_improvement_pct": statistics.median(differences),
                "bootstrap_95_lower_pct": lower,
                "bootstrap_95_upper_pct": upper,
                "wins": sum(value > tolerance for value in differences),
                "ties": sum(abs(value) <= tolerance for value in differences),
                "losses": sum(value < -tolerance for value in differences),
            }
        )
    return output


def run_evaluation(args: argparse.Namespace) -> None:
    root = Path(__file__).resolve().parents[1]
    executable = args.executable.resolve()
    prediction_file = args.prediction_file.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    logger, event_path = configure_logging(output_dir)
    traces = load_manifest(args.manifest.resolve(), args.patterns, args.max_traces)
    executable_argument = relative_to_root(executable, root)
    prediction_argument = relative_to_root(prediction_file, root)
    write_event(
        event_path,
        "evaluation_started",
        traces=len(traces),
        policies=list(POLICIES),
        minimum_lstm_probability=args.lstm_min_probability,
        minimum_lstm_predicted_get_ratio=args.lstm_min_predicted_get_ratio,
        lstm_inference_cost_ms=args.lstm_inference_cost_ms,
        bucket_size_mib=args.bucket_size_mib,
    )
    logger.info("formal policy evaluation: %d traces x %d policies", len(traces), len(POLICIES))

    results: List[Dict[str, object]] = []
    for trace_index, trace in enumerate(traces, start=1):
        source_windows = int(trace["windows"])
        evaluation_start = int(source_windows * args.evaluation_start_fraction)
        windows = int(source_windows * args.evaluation_end_fraction)
        if evaluation_start >= windows:
            raise ValueError("evaluation start must be before evaluation end")
        trace_results: List[Dict[str, object]] = []
        for policy in POLICIES:
            trace_output_dir = output_dir / "runs" / trace["trace_id"]
            trace_output_dir.mkdir(parents=True, exist_ok=True)
            result_path = trace_output_dir / "{}.csv".format(policy)
            result_argument = relative_to_root(result_path, root)
            command = [
                str(executable_argument),
                "--policy",
                policy,
                "--trace-id",
                trace["trace_id"],
                "--workload-pattern",
                trace["pattern"],
                "--nodes",
                "5",
                "--buckets",
                "64",
                "--entities",
                "10",
                "--windows",
                str(windows),
                "--requests",
                trace["requests_per_window"],
                "--node-capacity",
                trace["node_capacity"],
                "--hotspot-duration",
                trace["hotspot_duration"],
                "--read-ratio",
                trace["read_ratio"],
                "--hotspot-share",
                trace["hotspot_share"],
                "--threshold",
                trace["severe_get_threshold"],
                "--seed",
                trace["seed"],
                "--window-seconds",
                trace["window_seconds"],
                "--evaluation-start-window",
                str(evaluation_start),
                "--replica-ttl",
                str(args.replica_ttl),
                "--bucket-size-mib",
                str(args.bucket_size_mib),
                "--copy-bandwidth-mib-per-ms",
                str(args.copy_bandwidth_mib_per_ms),
                "--metadata-cost-ms",
                str(args.metadata_cost_ms),
                "--output",
                str(result_argument),
            ]
            if policy == "lstm":
                command.extend(
                    [
                        "--prediction-file",
                        str(prediction_argument),
                        "--lstm-min-probability",
                        str(args.lstm_min_probability),
                        "--lstm-min-predicted-get-ratio",
                        str(args.lstm_min_predicted_get_ratio),
                        "--lstm-inference-cost-ms",
                        str(args.lstm_inference_cost_ms),
                    ]
                )
            started = time.perf_counter()
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
            elapsed = time.perf_counter() - started
            summary = parse_summary(completed.stdout)
            trace_results.append(
                {
                    "trace_id": trace["trace_id"],
                    "pattern": trace["pattern"],
                    "seed": int(trace["seed"]),
                    "policy": policy,
                    "evaluated_windows": int(summary["windows"]),
                    "total_completion_ms": float(summary["total_completion_ms"]),
                    "max_node_load_ratio": float(summary["max_node_load_ratio"]),
                    "copies": int(summary["copies"]),
                    "wall_seconds": elapsed,
                    "result_csv": str(result_path),
                }
            )

        no_action_time = float(trace_results[0]["total_completion_ms"])
        for result in trace_results:
            result["improvement_vs_no_action_pct"] = (
                (no_action_time - float(result["total_completion_ms"])) / no_action_time * 100.0
            )
            results.append(result)
            write_event(event_path, "policy_completed", **result)
        logger.info(
            "[%02d/%02d] %-24s %-8s | reactive %+7.2f%% recent %+7.2f%% lstm %+7.2f%%",
            trace_index,
            len(traces),
            trace["trace_id"],
            trace["pattern"],
            trace_results[1]["improvement_vs_no_action_pct"],
            trace_results[2]["improvement_vs_no_action_pct"],
            trace_results[3]["improvement_vs_no_action_pct"],
        )

    result_fields = (
        "trace_id",
        "pattern",
        "seed",
        "policy",
        "evaluated_windows",
        "total_completion_ms",
        "improvement_vs_no_action_pct",
        "max_node_load_ratio",
        "copies",
        "wall_seconds",
        "result_csv",
    )
    write_csv(output_dir / "summary.csv", results, result_fields)
    aggregates = aggregate_results(results)
    aggregate_fields = (
        "pattern",
        "policy",
        "runs",
        "mean_completion_ms",
        "median_completion_ms",
        "mean_improvement_vs_no_action_pct",
        "median_improvement_vs_no_action_pct",
        "mean_copies",
        "wins_vs_no_action",
    )
    write_csv(output_dir / "aggregate.csv", aggregates, aggregate_fields)
    pairwise = pairwise_results(results)
    pairwise_fields = (
        "policy",
        "comparator",
        "traces",
        "mean_relative_improvement_pct",
        "median_relative_improvement_pct",
        "bootstrap_95_lower_pct",
        "bootstrap_95_upper_pct",
        "wins",
        "ties",
        "losses",
    )
    write_csv(output_dir / "pairwise.csv", pairwise, pairwise_fields)
    write_event(event_path, "evaluation_completed", runs=len(results))
    logger.info("summary=%s", output_dir / "summary.csv")
    logger.info("aggregate=%s", output_dir / "aggregate.csv")
    logger.info("pairwise=%s", output_dir / "pairwise.csv")


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
        default=root / "data" / "predictions" / "formal-test.csv",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=root / "results" / "formal-policy-evaluation",
    )
    parser.add_argument("--patterns", nargs="*", default=[])
    parser.add_argument("--max-traces", type=int, default=0)
    parser.add_argument("--replica-ttl", type=int, default=3)
    parser.add_argument("--bucket-size-mib", type=float, default=4.0)
    parser.add_argument("--copy-bandwidth-mib-per-ms", type=float, default=1.5)
    parser.add_argument("--metadata-cost-ms", type=float, default=0.08)
    parser.add_argument("--lstm-min-probability", type=float, default=0.70)
    parser.add_argument("--lstm-min-predicted-get-ratio", type=float, default=1.0)
    parser.add_argument("--lstm-inference-cost-ms", type=float, default=0.0)
    parser.add_argument("--evaluation-start-fraction", type=float, default=0.85)
    parser.add_argument("--evaluation-end-fraction", type=float, default=1.0)
    return parser.parse_args()


if __name__ == "__main__":
    run_evaluation(parse_args())
