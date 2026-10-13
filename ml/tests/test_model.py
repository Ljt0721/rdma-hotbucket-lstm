from __future__ import annotations

import unittest

import torch

from ml.model import HotBucketLSTM
from ml.train import hotspot_aware_regression_loss


class ModelTests(unittest.TestCase):
    def test_model_scores_each_bucket_sequence(self) -> None:
        model = HotBucketLSTM(input_size=5, hidden_size=16)
        hot_logit, get_share = model(torch.zeros(4, 6, 5))

        self.assertEqual(tuple(hot_logit.shape), (4,))
        self.assertEqual(tuple(get_share.shape), (4,))
        self.assertTrue(torch.all(get_share >= 0.0))
        self.assertTrue(torch.all(get_share <= 1.0))

    def test_severe_underprediction_can_receive_more_loss_weight(self) -> None:
        true_share = torch.tensor([0.5, 0.5])
        severe = torch.tensor([1.0, 0.0])
        underpredicted = torch.tensor([0.0, 0.49])
        overpredicted = torch.tensor([1.0, 0.49])

        under_loss = hotspot_aware_regression_loss(
            underpredicted, true_share, severe, 2.0, 4.0
        )
        over_loss = hotspot_aware_regression_loss(
            overpredicted, true_share, severe, 2.0, 4.0
        )

        self.assertGreater(float(under_loss), float(over_loss))

    def test_conditional_regression_ignores_non_hotspot_samples(self) -> None:
        true_share = torch.tensor([0.5, 0.1])
        severe = torch.tensor([1.0, 0.0])
        first_prediction = torch.tensor([0.4, 0.0])
        second_prediction = torch.tensor([0.4, 1.0])

        first_loss = hotspot_aware_regression_loss(
            first_prediction, true_share, severe, 1.0, 1.0, True
        )
        second_loss = hotspot_aware_regression_loss(
            second_prediction, true_share, severe, 1.0, 1.0, True
        )

        self.assertAlmostEqual(float(first_loss), float(second_loss), places=7)


if __name__ == "__main__":
    unittest.main()
