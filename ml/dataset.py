"""Build leak-free LSTM samples from policy-independent bucket traces."""

from __future__ import annotations

import csv
import json
from collections import defaultdict
from pathlib import Path
from typing import DefaultDict, Dict, List, Optional, Sequence, Tuple

import numpy as np


FEATURE_NAMES = (
    "get_count",
    "put_count",
    "read_ratio",
    "request_share",
    "home_node_logical_load_ratio",
)

REQUIRED_COLUMNS = {
    "trace_id",
    "pattern",
    "window_id",
    "bucket_id",
    "requests_per_window",
    "severe_get_threshold",
    *FEATURE_NAMES,
}


def _load_rows(paths: Sequence[Path]) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for path in paths:
        with path.open("r", newline="", encoding="utf-8") as source:
            reader = csv.DictReader(source)
            columns = set(reader.fieldnames or [])
            if not REQUIRED_COLUMNS.issubset(columns):
                missing = sorted(REQUIRED_COLUMNS - columns)
                raise ValueError("{} is missing columns: {}".format(path, ", ".join(missing)))
            rows.extend(reader)
    if not rows:
        raise ValueError("no trace rows were loaded")
    return rows


def discover_trace_files(raw_dir: Path) -> List[Path]:
    paths = sorted(path for path in raw_dir.glob("*.csv") if path.name != "manifest.csv")
    if not paths:
        raise FileNotFoundError("no trace CSV files found under {}".format(raw_dir))
    return paths


def _sample_arrays(
    samples: List[Dict[str, object]], sequence_length: int
) -> Dict[str, np.ndarray]:
    feature_count = len(FEATURE_NAMES)
    if samples:
        x = np.asarray([sample["x"] for sample in samples], dtype=np.float32)
    else:
        x = np.empty((0, sequence_length, feature_count), dtype=np.float32)
    return {
        "x": x,
        "y_hot": np.asarray([sample["y_hot"] for sample in samples], dtype=np.float32),
        "y_get_share": np.asarray(
            [sample["y_get_share"] for sample in samples], dtype=np.float32
        ),
        "trace_id": np.asarray([sample["trace_id"] for sample in samples], dtype=str),
        "pattern": np.asarray([sample["pattern"] for sample in samples], dtype=str),
        "bucket_id": np.asarray([sample["bucket_id"] for sample in samples], dtype=np.int64),
        "target_window": np.asarray(
            [sample["target_window"] for sample in samples], dtype=np.int64
        ),
        "target_get_count": np.asarray(
            [sample["target_get_count"] for sample in samples], dtype=np.float32
        ),
        "requests_per_window": np.asarray(
            [sample["requests_per_window"] for sample in samples], dtype=np.float32
        ),
        "threshold": np.asarray([sample["threshold"] for sample in samples], dtype=np.float32),
    }


def build_dataset(
    paths: Sequence[Path],
    output_dir: Path,
    sequence_length: int = 6,
    severe_get_threshold: Optional[int] = None,
    train_fraction: float = 0.70,
    validation_fraction: float = 0.15,
) -> Dict[str, object]:
    """Create chronological train/validation/test files and return metadata."""
    if sequence_length < 1:
        raise ValueError("sequence_length must be positive")
    if train_fraction <= 0 or validation_fraction <= 0:
        raise ValueError("train and validation fractions must be positive")
    if train_fraction + validation_fraction >= 1:
        raise ValueError("train and validation fractions must leave a test interval")

    rows = _load_rows(paths)
    groups: DefaultDict[Tuple[str, int], List[Dict[str, str]]] = defaultdict(list)
    trace_windows: DefaultDict[str, List[int]] = defaultdict(list)
    for row in rows:
        trace_id = row["trace_id"]
        bucket_id = int(row["bucket_id"])
        groups[(trace_id, bucket_id)].append(row)
        trace_windows[trace_id].append(int(row["window_id"]))

    cutoffs: Dict[str, Tuple[int, int]] = {}
    for trace_id, windows in trace_windows.items():
        window_count = max(windows) + 1
        train_end = max(sequence_length + 1, int(window_count * train_fraction))
        validation_end = max(train_end + 1, int(window_count * (train_fraction + validation_fraction)))
        if validation_end >= window_count:
            raise ValueError("trace {} is too short for all three splits".format(trace_id))
        cutoffs[trace_id] = (train_end, validation_end)

    split_samples: Dict[str, List[Dict[str, object]]] = {
        "train": [],
        "validation": [],
        "test": [],
    }
    skipped_non_contiguous = 0
    for (trace_id, bucket_id), bucket_rows in sorted(groups.items()):
        ordered = sorted(bucket_rows, key=lambda row: int(row["window_id"]))
        for target_index in range(sequence_length, len(ordered)):
            context = ordered[target_index - sequence_length : target_index]
            target = ordered[target_index]
            target_window = int(target["window_id"])
            expected_windows = list(range(target_window - sequence_length, target_window))
            actual_windows = [int(row["window_id"]) for row in context]
            if actual_windows != expected_windows:
                skipped_non_contiguous += 1
                continue

            threshold = (
                severe_get_threshold
                if severe_get_threshold is not None
                else int(target["severe_get_threshold"])
            )
            target_get_count = int(target["get_count"])
            requests_per_window = int(target["requests_per_window"])
            train_end, validation_end = cutoffs[trace_id]
            if target_window < train_end:
                split = "train"
            elif target_window < validation_end:
                split = "validation"
            else:
                split = "test"

            split_samples[split].append(
                {
                    "x": [[float(row[name]) for name in FEATURE_NAMES] for row in context],
                    "y_hot": float(target_get_count >= threshold),
                    "y_get_share": float(target_get_count) / float(requests_per_window),
                    "trace_id": trace_id,
                    "pattern": target["pattern"],
                    "bucket_id": bucket_id,
                    "target_window": target_window,
                    "target_get_count": target_get_count,
                    "requests_per_window": requests_per_window,
                    "threshold": threshold,
                }
            )

    arrays = {
        name: _sample_arrays(samples, sequence_length)
        for name, samples in split_samples.items()
    }
    if arrays["train"]["x"].shape[0] == 0:
        raise ValueError("training split contains no samples")

    training_x = arrays["train"]["x"]
    feature_mean = training_x.mean(axis=(0, 1), dtype=np.float64).astype(np.float32)
    feature_std = training_x.std(axis=(0, 1), dtype=np.float64).astype(np.float32)
    feature_std[feature_std < 1e-8] = 1.0
    for split in arrays.values():
        split["x"] = ((split["x"] - feature_mean) / feature_std).astype(np.float32)

    output_dir.mkdir(parents=True, exist_ok=True)
    for name, split in arrays.items():
        np.savez_compressed(output_dir / "{}.npz".format(name), **split)

    split_summary: Dict[str, Dict[str, int]] = {}
    for name, split in arrays.items():
        positive = int(split["y_hot"].sum())
        count = int(split["y_hot"].shape[0])
        split_summary[name] = {
            "samples": count,
            "severe_hotspots": positive,
            "non_hotspots": count - positive,
        }
    metadata: Dict[str, object] = {
        "sequence_length": sequence_length,
        "prediction_horizon_windows": 1,
        "observation_window_seconds": 30,
        "feature_names": list(FEATURE_NAMES),
        "feature_mean": feature_mean.tolist(),
        "feature_std": feature_std.tolist(),
        "split_rule": "chronological targets: first 70%, next 15%, final 15% per trace",
        "label_rule": (
            "future GET count >= command-line threshold"
            if severe_get_threshold is not None
            else "future GET count >= severe_get_threshold recorded in each trace"
        ),
        "source_files": [str(path) for path in paths],
        "trace_count": len(trace_windows),
        "bucket_series_count": len(groups),
        "skipped_non_contiguous_samples": skipped_non_contiguous,
        "splits": split_summary,
    }
    with (output_dir / "metadata.json").open("w", encoding="utf-8") as output:
        json.dump(metadata, output, indent=2)
    return metadata


def load_split(dataset_dir: Path, name: str) -> Dict[str, np.ndarray]:
    path = dataset_dir / "{}.npz".format(name)
    with np.load(path, allow_pickle=False) as stored:
        return {key: stored[key] for key in stored.files}
