from __future__ import annotations

import csv
import tempfile
import unittest
from pathlib import Path

import numpy as np

from ml.dataset import FEATURE_NAMES, build_dataset, load_split


class DatasetTests(unittest.TestCase):
    def test_dataset_uses_future_targets_and_chronological_splits(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            trace_path = root / "trace.csv"
            columns = [
                "trace_id",
                "pattern",
                "window_id",
                "bucket_id",
                "requests_per_window",
                "severe_get_threshold",
                *FEATURE_NAMES,
            ]
            with trace_path.open("w", newline="", encoding="utf-8") as output:
                writer = csv.DictWriter(output, fieldnames=columns)
                writer.writeheader()
                for window in range(20):
                    for bucket in (1, 2):
                        gets = window * 100 if bucket == 1 else 50
                        writer.writerow(
                            {
                                "trace_id": "test-trace",
                                "pattern": "ramp",
                                "window_id": window,
                                "bucket_id": bucket,
                                "requests_per_window": 2000,
                                "severe_get_threshold": 1200,
                                "get_count": gets,
                                "put_count": 20,
                                "read_ratio": gets / max(1, gets + 20),
                                "request_share": (gets + 20) / 2000,
                                "home_node_logical_load_ratio": (gets + 20) / 1000,
                            }
                        )

            output_dir = root / "dataset"
            metadata = build_dataset(
                paths=[trace_path],
                output_dir=output_dir,
                sequence_length=3,
                train_fraction=0.50,
                validation_fraction=0.25,
            )
            training = load_split(output_dir, "train")
            validation = load_split(output_dir, "validation")
            test = load_split(output_dir, "test")

            self.assertEqual(training["x"].shape[1:], (3, len(FEATURE_NAMES)))
            self.assertTrue(np.all(training["target_window"] < 10))
            self.assertTrue(np.all((validation["target_window"] >= 10) & (validation["target_window"] < 15)))
            self.assertTrue(np.all(test["target_window"] >= 15))
            hot_index = np.where(
                (test["bucket_id"] == 1) & (test["target_window"] == 15)
            )[0][0]
            self.assertEqual(test["target_get_count"][hot_index], 1500)
            self.assertEqual(test["y_hot"][hot_index], 1)
            self.assertEqual(metadata["trace_count"], 1)
            self.assertEqual(metadata["skipped_non_contiguous_samples"], 0)


if __name__ == "__main__":
    unittest.main()
