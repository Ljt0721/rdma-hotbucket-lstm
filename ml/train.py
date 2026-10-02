"""Train and evaluate the shared hot-bucket LSTM."""

from __future__ import annotations

import argparse
import csv
import json
import logging
import platform
import random
import sys
import time
from pathlib import Path
from typing import Dict, List, Tuple

import numpy as np
import torch
from torch import nn
from torch.utils.data import DataLoader, TensorDataset

from ml.dataset import load_split
from ml.model import HotBucketLSTM


def configure_logging(output_dir: Path) -> Tuple[logging.Logger, Path]:
    logger = logging.getLogger("hotbucket.training")
    logger.setLevel(logging.INFO)
    logger.handlers.clear()
    logger.propagate = False

    console = logging.StreamHandler(sys.stdout)
    console.setFormatter(logging.Formatter("%(message)s"))
    file_handler = logging.FileHandler(
        output_dir / "training.log", mode="w", encoding="utf-8"
    )
    file_handler.setFormatter(
        logging.Formatter("%(asctime)s %(levelname)s %(message)s", "%Y-%m-%d %H:%M:%S")
    )
    logger.addHandler(console)
    logger.addHandler(file_handler)

    event_path = output_dir / "events.jsonl"
    event_path.unlink(missing_ok=True)
    return logger, event_path


def write_event(event_path: Path, event_type: str, **fields: object) -> None:
    event = {
        "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "event": event_type,
        **fields,
    }
    with event_path.open("a", encoding="utf-8") as output:
        output.write(json.dumps(event, ensure_ascii=True) + "\n")


def choose_device(requested: str) -> torch.device:
    if requested == "auto":
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    if requested == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA was requested but is not available")
    return torch.device(requested)


def make_loader(
    split: Dict[str, np.ndarray], batch_size: int, shuffle: bool
) -> DataLoader:
    dataset = TensorDataset(
        torch.from_numpy(split["x"]).float(),
        torch.from_numpy(split["y_hot"]).float(),
        torch.from_numpy(split["y_get_share"]).float(),
    )
    return DataLoader(dataset, batch_size=batch_size, shuffle=shuffle, num_workers=0)


def calculate_metrics(
    labels: np.ndarray,
    probabilities: np.ndarray,
    true_share: np.ndarray,
    predicted_share: np.ndarray,
) -> Dict[str, float]:
    predicted = probabilities >= 0.5
    actual = labels >= 0.5
    true_positive = int(np.logical_and(predicted, actual).sum())
    false_positive = int(np.logical_and(predicted, np.logical_not(actual)).sum())
    false_negative = int(np.logical_and(np.logical_not(predicted), actual).sum())
    true_negative = int(np.logical_and(np.logical_not(predicted), np.logical_not(actual)).sum())
    precision = true_positive / max(1, true_positive + false_positive)
    recall = true_positive / max(1, true_positive + false_negative)
    f1 = 2.0 * precision * recall / max(1e-12, precision + recall)
    accuracy = (true_positive + true_negative) / max(1, labels.size)
    return {
        "accuracy": accuracy,
        "precision": precision,
        "recall": recall,
        "f1": f1,
        "get_share_mae": float(np.mean(np.abs(predicted_share - true_share))),
        "true_positive": true_positive,
        "false_positive": false_positive,
        "false_negative": false_negative,
        "true_negative": true_negative,
    }


def hotspot_aware_regression_loss(
    predicted_share: torch.Tensor,
    true_share: torch.Tensor,
    severe_label: torch.Tensor,
    severe_weight: float,
    underprediction_weight: float,
) -> torch.Tensor:
    per_sample = nn.functional.smooth_l1_loss(
        predicted_share, true_share, reduction="none"
    )
    weights = 1.0 + (severe_weight - 1.0) * severe_label
    severe_underprediction = torch.logical_and(
        severe_label >= 0.5, predicted_share < true_share
    )
    weights = weights * torch.where(
        severe_underprediction,
        torch.as_tensor(underprediction_weight, device=weights.device),
        torch.ones_like(weights),
    )
    return (per_sample * weights).sum() / weights.sum().clamp_min(1.0)


def evaluate(
    model: HotBucketLSTM,
    loader: DataLoader,
    classification_loss: nn.Module,
    regression_weight: float,
    severe_regression_weight: float,
    underprediction_weight: float,
    device: torch.device,
) -> Tuple[float, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    model.eval()
    losses: List[float] = []
    labels: List[np.ndarray] = []
    probabilities: List[np.ndarray] = []
    true_shares: List[np.ndarray] = []
    predicted_shares: List[np.ndarray] = []
    with torch.no_grad():
        for x, y_hot, y_share in loader:
            x = x.to(device)
            y_hot = y_hot.to(device)
            y_share = y_share.to(device)
            hot_logit, predicted_share = model(x)
            loss = classification_loss(hot_logit, y_hot) + regression_weight * (
                hotspot_aware_regression_loss(
                    predicted_share,
                    y_share,
                    y_hot,
                    severe_regression_weight,
                    underprediction_weight,
                )
            )
            losses.append(float(loss.item()) * x.shape[0])
            labels.append(y_hot.cpu().numpy())
            probabilities.append(torch.sigmoid(hot_logit).cpu().numpy())
            true_shares.append(y_share.cpu().numpy())
            predicted_shares.append(predicted_share.cpu().numpy())
    sample_count = max(1, len(loader.dataset))
    return (
        sum(losses) / sample_count,
        np.concatenate(labels),
        np.concatenate(probabilities),
        np.concatenate(true_shares),
        np.concatenate(predicted_shares),
    )


def train(args: argparse.Namespace) -> Dict[str, object]:
    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)
    if torch.cuda.is_available():
        torch.cuda.manual_seed_all(args.seed)

    dataset_dir = args.dataset_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    logger, event_path = configure_logging(output_dir)
    with (dataset_dir / "metadata.json").open("r", encoding="utf-8") as source:
        metadata = json.load(source)
    train_split = load_split(dataset_dir, "train")
    validation_split = load_split(dataset_dir, "validation")
    test_split = load_split(dataset_dir, "test")
    if train_split["y_hot"].sum() == 0:
        raise ValueError("training data has no severe-hotspot examples")

    device = choose_device(args.device)
    model = HotBucketLSTM(
        input_size=train_split["x"].shape[2],
        hidden_size=args.hidden_size,
        num_layers=args.layers,
        dropout=args.dropout,
    ).to(device)
    training_loader = make_loader(train_split, args.batch_size, shuffle=True)
    validation_loader = make_loader(validation_split, args.batch_size, shuffle=False)
    test_loader = make_loader(test_split, args.batch_size, shuffle=False)

    positive = float(train_split["y_hot"].sum())
    negative = float(train_split["y_hot"].shape[0] - positive)
    pos_weight = torch.tensor(
        negative / positive * args.positive_class_weight_scale,
        dtype=torch.float32,
        device=device,
    )
    classification_loss = nn.BCEWithLogitsLoss(pos_weight=pos_weight)
    optimizer = torch.optim.Adam(
        model.parameters(), lr=args.learning_rate, weight_decay=args.weight_decay
    )
    model_parameters = sum(parameter.numel() for parameter in model.parameters())
    run_settings = {
        key: str(value) if isinstance(value, Path) else value
        for key, value in vars(args).items()
    }
    logger.info("Hot-bucket LSTM training")
    logger.info(
        "device=%s parameters=%d train=%d validation=%d test=%d",
        device,
        model_parameters,
        len(training_loader.dataset),
        len(validation_loader.dataset),
        len(test_loader.dataset),
    )
    logger.info(
        "sequence=%d windows horizon=%d window features=%s",
        metadata["sequence_length"],
        metadata["prediction_horizon_windows"],
        ",".join(metadata["feature_names"]),
    )
    write_event(
        event_path,
        "run_started",
        settings=run_settings,
        device=str(device),
        python=platform.python_version(),
        torch=torch.__version__,
        cuda_available=torch.cuda.is_available(),
        model_parameters=model_parameters,
        dataset_splits=metadata["splits"],
    )

    best_validation_loss = float("inf")
    best_epoch = 0
    best_state: Dict[str, torch.Tensor] = {}
    epochs_without_improvement = 0
    history: List[Dict[str, float]] = []

    for epoch in range(1, args.epochs + 1):
        epoch_started = time.perf_counter()
        model.train()
        total_training_loss = 0.0
        for x, y_hot, y_share in training_loader:
            x = x.to(device)
            y_hot = y_hot.to(device)
            y_share = y_share.to(device)
            optimizer.zero_grad(set_to_none=True)
            hot_logit, predicted_share = model(x)
            loss = classification_loss(hot_logit, y_hot) + args.regression_weight * (
                hotspot_aware_regression_loss(
                    predicted_share,
                    y_share,
                    y_hot,
                    args.severe_regression_weight,
                    args.underprediction_weight,
                )
            )
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), args.gradient_clip)
            optimizer.step()
            total_training_loss += float(loss.item()) * x.shape[0]

        training_loss = total_training_loss / len(training_loader.dataset)
        validation_result = evaluate(
            model,
            validation_loader,
            classification_loss,
            args.regression_weight,
            args.severe_regression_weight,
            args.underprediction_weight,
            device,
        )
        validation_loss = validation_result[0]
        validation_metrics = calculate_metrics(
            validation_result[1],
            validation_result[2],
            validation_result[3],
            validation_result[4],
        )
        epoch_seconds = time.perf_counter() - epoch_started
        examples_per_second = len(training_loader.dataset) / max(epoch_seconds, 1e-9)
        history.append(
            {
                "epoch": float(epoch),
                "training_loss": training_loss,
                "validation_loss": validation_loss,
                "validation_precision": validation_metrics["precision"],
                "validation_recall": validation_metrics["recall"],
                "validation_f1": validation_metrics["f1"],
                "validation_get_share_mae": validation_metrics["get_share_mae"],
                "epoch_seconds": epoch_seconds,
                "examples_per_second": examples_per_second,
            }
        )
        logger.info(
            "epoch %03d/%03d | train %.5f | val %.5f | P %.3f R %.3f F1 %.3f "
            "| share_MAE %.4f | %.2fs",
            epoch,
            args.epochs,
            training_loss,
            validation_loss,
            validation_metrics["precision"],
            validation_metrics["recall"],
            validation_metrics["f1"],
            validation_metrics["get_share_mae"],
            epoch_seconds,
        )
        write_event(
            event_path,
            "epoch_completed",
            epoch=epoch,
            training_loss=training_loss,
            validation_loss=validation_loss,
            validation_metrics=validation_metrics,
            epoch_seconds=epoch_seconds,
            examples_per_second=examples_per_second,
        )

        if validation_loss < best_validation_loss - args.minimum_delta:
            best_validation_loss = validation_loss
            best_epoch = epoch
            best_state = {
                name: value.detach().cpu().clone() for name, value in model.state_dict().items()
            }
            epochs_without_improvement = 0
            torch.save(
                {
                    "model_state_dict": best_state,
                    "model_config": {
                        "input_size": int(train_split["x"].shape[2]),
                        "hidden_size": args.hidden_size,
                        "num_layers": args.layers,
                        "dropout": args.dropout,
                    },
                    "feature_names": metadata["feature_names"],
                    "feature_mean": metadata["feature_mean"],
                    "feature_std": metadata["feature_std"],
                    "sequence_length": metadata["sequence_length"],
                    "prediction_horizon_windows": metadata[
                        "prediction_horizon_windows"
                    ],
                    "probability_threshold": 0.5,
                    "training_seed": args.seed,
                    "best_epoch": epoch,
                    "best_validation_loss": best_validation_loss,
                },
                output_dir / "hotbucket_lstm.pt",
            )
            logger.info("  saved new best checkpoint (validation loss %.5f)", validation_loss)
            write_event(
                event_path,
                "best_checkpoint_saved",
                epoch=epoch,
                validation_loss=validation_loss,
            )
        else:
            epochs_without_improvement += 1
            if epochs_without_improvement >= args.patience:
                logger.info(
                    "early stopping at epoch %d after %d epochs without improvement",
                    epoch,
                    args.patience,
                )
                write_event(
                    event_path,
                    "early_stopping",
                    epoch=epoch,
                    patience=args.patience,
                )
                break

    model.load_state_dict(best_state)
    model.to(device)
    best_validation_result = evaluate(
        model,
        validation_loader,
        classification_loss,
        args.regression_weight,
        args.severe_regression_weight,
        args.underprediction_weight,
        device,
    )
    best_validation_metrics = calculate_metrics(
        best_validation_result[1],
        best_validation_result[2],
        best_validation_result[3],
        best_validation_result[4],
    )
    best_validation_metrics["loss"] = best_validation_result[0]
    test_result = evaluate(
        model,
        test_loader,
        classification_loss,
        args.regression_weight,
        args.severe_regression_weight,
        args.underprediction_weight,
        device,
    )
    test_loss, labels, probabilities, true_shares, predicted_shares = test_result
    test_metrics = calculate_metrics(labels, probabilities, true_shares, predicted_shares)
    test_metrics["loss"] = test_loss
    metrics_by_pattern: Dict[str, Dict[str, float]] = {}
    for pattern in sorted(set(test_split["pattern"].tolist())):
        mask = test_split["pattern"] == pattern
        metrics_by_pattern[str(pattern)] = calculate_metrics(
            labels[mask],
            probabilities[mask],
            true_shares[mask],
            predicted_shares[mask],
        )

    checkpoint = {
        "model_state_dict": best_state,
        "model_config": {
            "input_size": int(train_split["x"].shape[2]),
            "hidden_size": args.hidden_size,
            "num_layers": args.layers,
            "dropout": args.dropout,
        },
        "feature_names": metadata["feature_names"],
        "feature_mean": metadata["feature_mean"],
        "feature_std": metadata["feature_std"],
        "sequence_length": metadata["sequence_length"],
        "prediction_horizon_windows": metadata["prediction_horizon_windows"],
        "probability_threshold": 0.5,
        "training_seed": args.seed,
        "best_epoch": best_epoch,
        "best_validation_loss": best_validation_loss,
    }
    torch.save(checkpoint, output_dir / "hotbucket_lstm.pt")

    with (output_dir / "training_history.csv").open(
        "w", newline="", encoding="utf-8"
    ) as output:
        writer = csv.DictWriter(output, fieldnames=list(history[0].keys()))
        writer.writeheader()
        writer.writerows(history)

    with (output_dir / "test_predictions.csv").open(
        "w", newline="", encoding="utf-8"
    ) as output:
        fieldnames = [
            "trace_id",
            "pattern",
            "bucket_id",
            "target_window",
            "actual_severe_hotspot",
            "predicted_hotspot_probability",
            "actual_get_count",
            "predicted_get_count",
        ]
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        for index in range(labels.shape[0]):
            writer.writerow(
                {
                    "trace_id": test_split["trace_id"][index],
                    "pattern": test_split["pattern"][index],
                    "bucket_id": int(test_split["bucket_id"][index]),
                    "target_window": int(test_split["target_window"][index]),
                    "actual_severe_hotspot": int(labels[index]),
                    "predicted_hotspot_probability": float(probabilities[index]),
                    "actual_get_count": float(test_split["target_get_count"][index]),
                    "predicted_get_count": float(
                        predicted_shares[index] * test_split["requests_per_window"][index]
                    ),
                }
            )

    result: Dict[str, object] = {
        "device": str(device),
        "epochs_completed": len(history),
        "best_epoch": best_epoch,
        "best_validation_loss": best_validation_loss,
        "positive_class_weight": float(pos_weight.item()),
        "model_parameters": model_parameters,
        "validation": best_validation_metrics,
        "test": test_metrics,
        "test_by_pattern": metrics_by_pattern,
    }
    with (output_dir / "metrics.json").open("w", encoding="utf-8") as output:
        json.dump(result, output, indent=2)
    write_event(event_path, "run_completed", result=result)
    logger.info("training complete")
    logger.info(json.dumps(result, indent=2))
    logger.info("checkpoint=%s", output_dir / "hotbucket_lstm.pt")
    return result


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-dir", type=Path, default=root / "data" / "training")
    parser.add_argument("--output-dir", type=Path, default=root / "models" / "lstm")
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--hidden-size", type=int, default=32)
    parser.add_argument("--layers", type=int, default=1)
    parser.add_argument("--dropout", type=float, default=0.0)
    parser.add_argument("--learning-rate", type=float, default=1e-3)
    parser.add_argument("--regression-weight", type=float, default=0.25)
    parser.add_argument("--severe-regression-weight", type=float, default=1.0)
    parser.add_argument("--underprediction-weight", type=float, default=1.0)
    parser.add_argument("--positive-class-weight-scale", type=float, default=1.0)
    parser.add_argument("--weight-decay", type=float, default=0.0)
    parser.add_argument("--gradient-clip", type=float, default=1.0)
    parser.add_argument("--patience", type=int, default=6)
    parser.add_argument("--minimum-delta", type=float, default=1e-5)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    return parser.parse_args()


def main() -> None:
    train(parse_args())


if __name__ == "__main__":
    main()
