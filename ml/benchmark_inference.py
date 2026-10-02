"""Measure one low-frequency LSTM controller inference over ten buckets."""

from __future__ import annotations

import argparse
import json
import statistics
import time
from pathlib import Path
from typing import Dict

import numpy as np
import torch

from ml.dataset import load_split
from ml.model import HotBucketLSTM


def percentile(values: np.ndarray, quantile: float) -> float:
    return float(np.percentile(values, quantile))


def benchmark(args: argparse.Namespace) -> Dict[str, object]:
    device = torch.device(args.device)
    if args.device == "cpu":
        torch.set_num_threads(args.cpu_threads)
    checkpoint = torch.load(
        args.checkpoint.resolve(), map_location=device, weights_only=False
    )
    model = HotBucketLSTM(**checkpoint["model_config"])
    model.load_state_dict(checkpoint["model_state_dict"])
    model.to(device)
    model.eval()
    test = load_split(args.dataset_dir.resolve(), "test")
    x = torch.from_numpy(test["x"][: args.bucket_count]).float().to(device)

    def run_once() -> None:
        with torch.no_grad():
            hot_logit, get_share = model(x)
            torch.sigmoid(hot_logit)
            get_share.sum()
        if device.type == "cuda":
            torch.cuda.synchronize(device)

    for _ in range(args.warmup):
        run_once()
    durations = []
    for _ in range(args.iterations):
        started = time.perf_counter_ns()
        run_once()
        durations.append((time.perf_counter_ns() - started) / 1_000_000.0)

    measured = np.asarray(durations, dtype=np.float64)
    result: Dict[str, object] = {
        "device": str(device),
        "cpu_threads": args.cpu_threads if device.type == "cpu" else None,
        "bucket_count": args.bucket_count,
        "sequence_length": int(x.shape[1]),
        "features": int(x.shape[2]),
        "warmup_iterations": args.warmup,
        "measured_iterations": args.iterations,
        "mean_ms": statistics.fmean(durations),
        "median_ms": statistics.median(durations),
        "p95_ms": percentile(measured, 95),
        "p99_ms": percentile(measured, 99),
        "minimum_ms": float(measured.min()),
        "maximum_ms": float(measured.max()),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
    print(json.dumps(result, indent=2))
    print("result={}".format(args.output))
    return result


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--checkpoint",
        type=Path,
        default=root / "models" / "lstm" / "formal" / "hotbucket_lstm.pt",
    )
    parser.add_argument(
        "--dataset-dir",
        type=Path,
        default=root / "data" / "training" / "formal",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=root / "models" / "lstm" / "formal" / "inference_benchmark_cpu.json",
    )
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    parser.add_argument("--cpu-threads", type=int, default=1)
    parser.add_argument("--bucket-count", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--iterations", type=int, default=1000)
    return parser.parse_args()


if __name__ == "__main__":
    benchmark(parse_args())
