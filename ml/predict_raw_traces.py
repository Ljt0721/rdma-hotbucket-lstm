"""Run a frozen LSTM on new policy-independent raw trace CSV files."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path
from typing import DefaultDict, Dict, List, Tuple

import numpy as np
import torch
from torch.utils.data import DataLoader, TensorDataset

from ml.dataset import FEATURE_NAMES, REQUIRED_COLUMNS, discover_trace_files
from ml.model import HotBucketLSTM


def build_samples(
    paths: List[Path], sequence_length: int
) -> Tuple[np.ndarray, List[Dict[str, object]]]:
    groups: DefaultDict[Tuple[str, int], List[Dict[str, str]]] = defaultdict(list)
    for path in paths:
        with path.open("r", newline="", encoding="utf-8") as source:
            reader = csv.DictReader(source)
            columns = set(reader.fieldnames or [])
            if not REQUIRED_COLUMNS.issubset(columns):
                missing = sorted(REQUIRED_COLUMNS - columns)
                raise ValueError("{} is missing columns: {}".format(path, ", ".join(missing)))
            for row in reader:
                groups[(row["trace_id"], int(row["bucket_id"]))].append(row)

    inputs = []
    metadata: List[Dict[str, object]] = []
    for (trace_id, bucket_id), rows in sorted(groups.items()):
        ordered = sorted(rows, key=lambda row: int(row["window_id"]))
        for target_index in range(sequence_length, len(ordered)):
            context = ordered[target_index - sequence_length : target_index]
            target = ordered[target_index]
            target_window = int(target["window_id"])
            if [int(row["window_id"]) for row in context] != list(
                range(target_window - sequence_length, target_window)
            ):
                continue
            inputs.append([[float(row[name]) for name in FEATURE_NAMES] for row in context])
            metadata.append(
                {
                    "trace_id": trace_id,
                    "window_id": target_window,
                    "bucket_id": bucket_id,
                    "requests_per_window": int(target["requests_per_window"]),
                }
            )
    if not inputs:
        raise ValueError("no contiguous inference samples were found")
    return np.asarray(inputs, dtype=np.float32), metadata


def predict(args: argparse.Namespace) -> None:
    device = torch.device(
        "cuda" if args.device == "auto" and torch.cuda.is_available()
        else "cpu" if args.device == "auto"
        else args.device
    )
    checkpoint = torch.load(args.checkpoint.resolve(), map_location=device, weights_only=False)
    model = HotBucketLSTM(**checkpoint["model_config"])
    model.load_state_dict(checkpoint["model_state_dict"])
    model.to(device)
    model.eval()

    paths = discover_trace_files(args.raw_dir.resolve())
    x, rows = build_samples(paths, int(checkpoint["sequence_length"]))
    feature_mean = np.asarray(checkpoint["feature_mean"], dtype=np.float32)
    feature_std = np.asarray(checkpoint["feature_std"], dtype=np.float32)
    x = ((x - feature_mean) / feature_std).astype(np.float32)
    loader = DataLoader(
        TensorDataset(torch.from_numpy(x)),
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=0,
    )

    probabilities = []
    shares = []
    with torch.no_grad():
        for (batch,) in loader:
            logits, predicted_share = model(batch.to(device))
            probabilities.append(torch.sigmoid(logits).cpu().numpy())
            shares.append(predicted_share.cpu().numpy())
    probability = np.concatenate(probabilities)
    predicted_share = np.concatenate(shares)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=(
                "trace_id",
                "window_id",
                "bucket_id",
                "hotspot_probability",
                "predicted_gets",
            ),
        )
        writer.writeheader()
        for index, row in enumerate(rows):
            writer.writerow(
                {
                    "trace_id": row["trace_id"],
                    "window_id": row["window_id"],
                    "bucket_id": row["bucket_id"],
                    "hotspot_probability": float(probability[index]),
                    "predicted_gets": float(
                        predicted_share[index] * int(row["requests_per_window"])
                    ),
                }
            )
    print("device={}".format(device))
    print("traces={}".format(len({str(row["trace_id"]) for row in rows})))
    print("predictions={}".format(len(rows)))
    print("csv={}".format(args.output))


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--checkpoint",
        type=Path,
        default=root / "models" / "lstm" / "formal" / "hotbucket_lstm.pt",
    )
    parser.add_argument("--raw-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--batch-size", type=int, default=512)
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    return parser.parse_args()


if __name__ == "__main__":
    predict(parse_args())
