"""Export frozen LSTM predictions for the C++ cost-aware controller."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import numpy as np
import torch
from torch.utils.data import DataLoader, TensorDataset

from ml.dataset import load_split
from ml.model import HotBucketLSTM


def export_predictions(
    checkpoint_path: Path,
    dataset_dir: Path,
    output_path: Path,
    batch_size: int,
    device_name: str,
    split_name: str,
) -> None:
    device = torch.device(
        "cuda" if device_name == "auto" and torch.cuda.is_available() else
        "cpu" if device_name == "auto" else device_name
    )
    checkpoint = torch.load(checkpoint_path, map_location=device, weights_only=False)
    model = HotBucketLSTM(**checkpoint["model_config"])
    model.load_state_dict(checkpoint["model_state_dict"])
    model.to(device)
    model.eval()

    split = load_split(dataset_dir, split_name)
    loader = DataLoader(
        TensorDataset(torch.from_numpy(split["x"]).float()),
        batch_size=batch_size,
        shuffle=False,
        num_workers=0,
    )
    probabilities = []
    predicted_shares = []
    with torch.no_grad():
        for (x,) in loader:
            hot_logit, get_share = model(x.to(device))
            probabilities.append(torch.sigmoid(hot_logit).cpu().numpy())
            predicted_shares.append(get_share.cpu().numpy())

    probability = np.concatenate(probabilities)
    predicted_share = np.concatenate(predicted_shares)
    predicted_gets = predicted_share * split["requests_per_window"]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="", encoding="utf-8") as output:
        fields = (
            "trace_id",
            "window_id",
            "bucket_id",
            "hotspot_probability",
            "predicted_gets",
        )
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for index in range(probability.shape[0]):
            writer.writerow(
                {
                    "trace_id": split["trace_id"][index],
                    "window_id": int(split["target_window"][index]),
                    "bucket_id": int(split["bucket_id"][index]),
                    "hotspot_probability": float(probability[index]),
                    "predicted_gets": float(predicted_gets[index]),
                }
            )
    print("device={}".format(device))
    print("predictions={}".format(probability.shape[0]))
    print("split={}".format(split_name))
    print("traces={}".format(len(set(split["trace_id"].tolist()))))
    print("csv={}".format(output_path))


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
        default=root / "data" / "predictions" / "formal-test.csv",
    )
    parser.add_argument("--batch-size", type=int, default=512)
    parser.add_argument("--split", choices=("train", "validation", "test"), default="test")
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    export_predictions(
        checkpoint_path=args.checkpoint.resolve(),
        dataset_dir=args.dataset_dir.resolve(),
        output_path=args.output.resolve(),
        batch_size=args.batch_size,
        device_name=args.device,
        split_name=args.split,
    )


if __name__ == "__main__":
    main()
