#!/usr/bin/env python3
"""Attribute the LSTM versus recent-window completion-time difference."""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import DefaultDict, Dict, List, Tuple


def load_windows(path: Path) -> Dict[int, List[Dict[str, str]]]:
    windows: DefaultDict[int, List[Dict[str, str]]] = defaultdict(list)
    with path.open("r", newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            windows[int(row["window_id"])].append(row)
    return dict(windows)


def load_predictions(
    path: Path,
) -> Dict[Tuple[str, int, int], Tuple[float, float]]:
    predictions = {}
    with path.open("r", newline="", encoding="utf-8") as source:
        for row in csv.DictReader(source):
            key = (row["trace_id"], int(row["window_id"]), int(row["bucket_id"]))
            predictions[key] = (
                float(row["hotspot_probability"]),
                float(row["predicted_gets"]),
            )
    return predictions


def update_prediction_diagnostics(
    totals: DefaultDict[str, float],
    trace_id: str,
    actual_gets: Dict[int, Dict[int, int]],
    predictions: Dict[Tuple[str, int, int], Tuple[float, float]],
    severe_threshold: int,
    minimum_probability: float,
    minimum_predicted_get_ratio: float,
) -> None:
    previous_severe: Dict[int, bool] = defaultdict(bool)
    for window_id in sorted(actual_gets):
        for bucket_id, actual in actual_gets[window_id].items():
            prediction = predictions.get((trace_id, window_id, bucket_id))
            if prediction is None:
                continue
            probability, predicted_gets = prediction
            actual_severe = actual >= severe_threshold
            probability_pass = probability >= minimum_probability
            regression_pass = (
                predicted_gets >= severe_threshold * minimum_predicted_get_ratio
            )
            both_pass = probability_pass and regression_pass

            if actual_severe:
                totals["severe_predictions"] += 1
                totals["probability_pass_on_severe"] += int(probability_pass)
                totals["regression_pass_on_severe"] += int(regression_pass)
                totals["both_gates_pass_on_severe"] += int(both_pass)
                totals["classifier_pass_but_regression_blocked_on_severe"] += int(
                    probability_pass and not regression_pass
                )
                totals["severe_prediction_error_sum"] += predicted_gets - actual
                totals["severe_prediction_absolute_error_sum"] += abs(predicted_gets - actual)
                if not previous_severe[bucket_id]:
                    totals["severe_onsets"] += 1
                    totals["both_gates_pass_on_onset"] += int(both_pass)
                    totals["classifier_pass_but_regression_blocked_on_onset"] += int(
                        probability_pass and not regression_pass
                    )
            elif both_pass:
                totals["both_gates_false_positive"] += 1
            previous_severe[bucket_id] = actual_severe


def window_cost(rows: List[Dict[str, str]]) -> Dict[str, float]:
    completion_ms = float(rows[0]["window_completion_ms"])
    copy_cost_ms = sum(float(row["copy_cost_ms"]) for row in rows)
    control_overhead_ms = float(rows[0]["control_overhead_ms"])
    return {
        "completion_ms": completion_ms,
        "copy_cost_ms": copy_cost_ms,
        "control_overhead_ms": control_overhead_ms,
        "service_ms": completion_ms - copy_cost_ms - control_overhead_ms,
    }


def policy_diagnostics(
    windows: Dict[int, List[Dict[str, str]]],
    actual_gets: Dict[int, Dict[int, int]],
    severe_threshold: int,
    replica_ttl: int,
) -> Dict[str, int]:
    severe_bucket_windows = 0
    covered_severe_bucket_windows = 0
    copy_events = []
    attempted_but_not_applied = 0
    for window_id, rows in windows.items():
        for row in rows:
            bucket_id = int(row["bucket_id"])
            if actual_gets[window_id][bucket_id] >= severe_threshold:
                severe_bucket_windows += 1
                if int(row["replica_count"]) > 0:
                    covered_severe_bucket_windows += 1
            if row["decision_bucket"] and row["copy_applied"] != "1":
                attempted_but_not_applied += 1
            if row["copy_applied"] == "1":
                copy_events.append((window_id, bucket_id))

    useful_copies = 0
    proactive_copies = 0
    wasted_copies = 0
    for window_id, bucket_id in copy_events:
        severe_windows = [
            future_window
            for future_window in range(window_id, window_id + replica_ttl + 1)
            if future_window in actual_gets
            and actual_gets[future_window][bucket_id] >= severe_threshold
        ]
        if severe_windows:
            useful_copies += 1
            if severe_windows[0] > window_id:
                proactive_copies += 1
        else:
            wasted_copies += 1

    return {
        "severe_bucket_windows": severe_bucket_windows,
        "covered_severe_bucket_windows": covered_severe_bucket_windows,
        "missed_severe_bucket_windows": severe_bucket_windows - covered_severe_bucket_windows,
        "copy_events": len(copy_events),
        "useful_copies_within_ttl": useful_copies,
        "proactive_copies_before_threshold": proactive_copies,
        "wasted_copies_within_ttl": wasted_copies,
        "attempted_but_not_applied": attempted_but_not_applied,
    }


def analyze(args: argparse.Namespace) -> Dict[str, object]:
    with args.manifest.open("r", newline="", encoding="utf-8") as source:
        manifest = list(csv.DictReader(source))
    predictions = load_predictions(args.predictions)

    totals = {
        "completion_gap_ms": 0.0,
        "service_gap_ms": 0.0,
        "copy_cost_gap_ms": 0.0,
        "inference_gap_ms": 0.0,
    }
    recent_diagnostics: DefaultDict[str, int] = defaultdict(int)
    lstm_diagnostics: DefaultDict[str, int] = defaultdict(int)
    prediction_diagnostics: DefaultDict[str, float] = defaultdict(float)
    evaluated_windows = 0
    lstm_slower_windows = 0
    lstm_faster_windows = 0
    equal_windows = 0

    for trace in manifest:
        trace_dir = args.results_dir / "runs" / trace["trace_id"]
        no_action = load_windows(trace_dir / "no-action.csv")
        recent = load_windows(trace_dir / "recent-window.csv")
        lstm = load_windows(trace_dir / "lstm.csv")
        actual_gets = {
            window_id: {
                int(row["bucket_id"]): int(row["get_count"])
                for row in rows
            }
            for window_id, rows in no_action.items()
        }
        threshold = int(trace["severe_get_threshold"])
        update_prediction_diagnostics(
            prediction_diagnostics,
            trace["trace_id"],
            actual_gets,
            predictions,
            threshold,
            args.minimum_probability,
            args.minimum_predicted_get_ratio,
        )
        for key, value in policy_diagnostics(
            recent, actual_gets, threshold, args.replica_ttl
        ).items():
            recent_diagnostics[key] += value
        for key, value in policy_diagnostics(
            lstm, actual_gets, threshold, args.replica_ttl
        ).items():
            lstm_diagnostics[key] += value

        for window_id in sorted(recent):
            recent_cost = window_cost(recent[window_id])
            lstm_cost = window_cost(lstm[window_id])
            completion_gap = lstm_cost["completion_ms"] - recent_cost["completion_ms"]
            totals["completion_gap_ms"] += completion_gap
            totals["service_gap_ms"] += (
                lstm_cost["service_ms"] - recent_cost["service_ms"]
            )
            totals["copy_cost_gap_ms"] += (
                lstm_cost["copy_cost_ms"] - recent_cost["copy_cost_ms"]
            )
            totals["inference_gap_ms"] += (
                lstm_cost["control_overhead_ms"] -
                recent_cost["control_overhead_ms"]
            )
            evaluated_windows += 1
            if completion_gap > 1e-6:
                lstm_slower_windows += 1
            elif completion_gap < -1e-6:
                lstm_faster_windows += 1
            else:
                equal_windows += 1

    trace_count = len(manifest)
    completion_gap = totals["completion_gap_ms"]
    time_attribution = {
        **totals,
        "mean_completion_gap_ms_per_trace": completion_gap / trace_count,
        "mean_service_gap_ms_per_trace": totals["service_gap_ms"] / trace_count,
        "mean_copy_cost_gap_ms_per_trace": totals["copy_cost_gap_ms"] / trace_count,
        "mean_inference_gap_ms_per_trace": totals["inference_gap_ms"] / trace_count,
        "service_share_of_gap": totals["service_gap_ms"] / completion_gap,
        "copy_cost_share_of_gap": totals["copy_cost_gap_ms"] / completion_gap,
        "inference_share_of_gap": totals["inference_gap_ms"] / completion_gap,
    }
    for diagnostics in (recent_diagnostics, lstm_diagnostics):
        diagnostics["severe_window_coverage"] = (
            diagnostics["covered_severe_bucket_windows"]
            / diagnostics["severe_bucket_windows"]
        )
        diagnostics["useful_copy_rate"] = (
            diagnostics["useful_copies_within_ttl"] / diagnostics["copy_events"]
        )
    severe_predictions = prediction_diagnostics["severe_predictions"]
    severe_onsets = prediction_diagnostics["severe_onsets"]
    prediction_diagnostics.update(
        {
            "probability_recall_on_severe": (
                prediction_diagnostics["probability_pass_on_severe"] / severe_predictions
            ),
            "regression_recall_on_severe": (
                prediction_diagnostics["regression_pass_on_severe"] / severe_predictions
            ),
            "combined_gate_recall_on_severe": (
                prediction_diagnostics["both_gates_pass_on_severe"] / severe_predictions
            ),
            "combined_gate_recall_on_onset": (
                prediction_diagnostics["both_gates_pass_on_onset"] / severe_onsets
            ),
            "mean_prediction_bias_on_severe_gets": (
                prediction_diagnostics["severe_prediction_error_sum"] / severe_predictions
            ),
            "mae_on_severe_gets": (
                prediction_diagnostics["severe_prediction_absolute_error_sum"]
                / severe_predictions
            ),
        }
    )
    result: Dict[str, object] = {
        "trace_count": trace_count,
        "evaluated_windows": evaluated_windows,
        "time_attribution": time_attribution,
        "window_comparison": {
            "lstm_slower_windows": lstm_slower_windows,
            "lstm_faster_windows": lstm_faster_windows,
            "equal_windows": equal_windows,
        },
        "recent_window": dict(recent_diagnostics),
        "lstm": dict(lstm_diagnostics),
        "lstm_prediction_gates": dict(prediction_diagnostics),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
    print(json.dumps(result, indent=2))
    print("report={}".format(args.output))
    return result


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=root / "data" / "traces" / "formal" / "raw" / "manifest.csv",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=root / "results" / "formal-policy-evaluation",
    )
    parser.add_argument(
        "--predictions",
        type=Path,
        default=root / "data" / "predictions" / "formal-test.csv",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=root / "results" / "formal-policy-evaluation" / "bottleneck.json",
    )
    parser.add_argument("--replica-ttl", type=int, default=3)
    parser.add_argument("--minimum-probability", type=float, default=0.70)
    parser.add_argument("--minimum-predicted-get-ratio", type=float, default=1.0)
    return parser.parse_args()


if __name__ == "__main__":
    analyze(parse_args())
