"""Convert raw trace CSV files into chronological LSTM datasets."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from ml.dataset import build_dataset, discover_trace_files


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw-dir", type=Path, default=root / "data" / "traces" / "raw")
    parser.add_argument("--output-dir", type=Path, default=root / "data" / "training")
    parser.add_argument("--sequence-length", type=int, default=6)
    parser.add_argument(
        "--severe-get-threshold",
        type=int,
        default=None,
        help="override thresholds stored in traces",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    paths = discover_trace_files(args.raw_dir.resolve())
    metadata = build_dataset(
        paths=paths,
        output_dir=args.output_dir.resolve(),
        sequence_length=args.sequence_length,
        severe_get_threshold=args.severe_get_threshold,
    )
    print(json.dumps(metadata["splits"], indent=2))
    print("dataset={}".format(args.output_dir.resolve()))


if __name__ == "__main__":
    main()
