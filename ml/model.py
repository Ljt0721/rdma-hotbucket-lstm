"""Small shared LSTM used to score each bucket's next observation window."""

from __future__ import annotations

from typing import Tuple

import torch
from torch import nn


class HotBucketLSTM(nn.Module):
    """Predict severe-hotspot probability and future GET share for one bucket sequence."""

    def __init__(
        self,
        input_size: int,
        hidden_size: int = 32,
        num_layers: int = 1,
        dropout: float = 0.0,
    ) -> None:
        super().__init__()
        effective_dropout = dropout if num_layers > 1 else 0.0
        self.lstm = nn.LSTM(
            input_size=input_size,
            hidden_size=hidden_size,
            num_layers=num_layers,
            dropout=effective_dropout,
            batch_first=True,
        )
        self.hotspot_head = nn.Linear(hidden_size, 1)
        self.get_share_head = nn.Sequential(nn.Linear(hidden_size, 1), nn.Sigmoid())

    def forward(self, x: torch.Tensor) -> Tuple[torch.Tensor, torch.Tensor]:
        sequence_output, _ = self.lstm(x)
        final_state = sequence_output[:, -1, :]
        hot_logit = self.hotspot_head(final_state).squeeze(-1)
        get_share = self.get_share_head(final_state).squeeze(-1)
        return hot_logit, get_share
