"""The policy/value network.

Fully convolutional over the board, so one set of weights handles every size
from 10x10 to 50x50 without retraining or reshaping. Global pooling turns the
spatial features into a fixed-width embedding, and the whole-board scalars are
concatenated there, because pooling discards exactly the quantity the game is
scored on.

The policy head does not emit a move. It scores each *candidate turn* the
environment proposes, from the board embedding together with that candidate's
own features, and the logits are masked down to the candidates that exist. So
the network learns which proposal to trust rather than how to spell out a
legal compound turn, and it can overrule the heuristic's ranking where that
ranking is wrong.

Written with plain torch layers; nothing here comes from a game, search or
agent library.
"""

from __future__ import annotations

from typing import Dict, List, Optional, Tuple

import numpy as np
import torch
import torch.nn as nn

from .features import CANDIDATE_FEATURES, PLANES, SCALARS

NEG_INF = -1e9


class ConvBlock(nn.Module):
    def __init__(self, channels: int, kernel: int):
        super().__init__()
        self.conv = nn.Conv2d(channels, channels, kernel, padding=kernel // 2)
        self.norm = nn.GroupNorm(1, channels)
        self.activation = nn.ReLU(inplace=True)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # Residual: keeps gradients alive through a deeper trunk.
        return self.activation(x + self.norm(self.conv(x)))


class PolicyValueNet(nn.Module):
    def __init__(
        self,
        channels: int = 32,
        blocks: int = 4,
        kernel: int = 3,
        head_hidden: int = 128,
        max_pool: bool = False,
    ):
        super().__init__()
        self.max_pool = max_pool
        self.stem = nn.Sequential(
            nn.Conv2d(PLANES, channels, kernel, padding=kernel // 2),
            nn.GroupNorm(1, channels),
            nn.ReLU(inplace=True),
        )
        self.trunk = nn.Sequential(*[ConvBlock(channels, kernel) for _ in range(blocks)])
        self.pool = nn.AdaptiveAvgPool2d(1)
        # Optional max pooling, off by default. It was added on the theory
        # that mean density cannot tell one large stack from the same
        # creatures spread thin, which the 1.5x threshold cares about, and
        # then measured: it changed warm-start accuracy by 0.002-0.004, which
        # is noise, and it raised the logit difference under reflection from
        # ~1e-4 to ~2e-2. Max picks the single largest activation, which can
        # be a padding-edge artefact, so unlike averaging it notices where the
        # board sits inside its padding. No gain, and it costs the free
        # orientation invariance; see docs/paper_notes.md.
        self.peak = nn.AdaptiveMaxPool2d(1) if max_pool else None
        pooled_width = channels * (2 if max_pool else 1)

        self.embed = nn.Sequential(
            nn.Linear(pooled_width + SCALARS, head_hidden),
            nn.ReLU(inplace=True),
        )
        self.policy = nn.Sequential(
            nn.Linear(head_hidden + CANDIDATE_FEATURES, head_hidden),
            nn.ReLU(inplace=True),
            nn.Linear(head_hidden, 1),
        )
        self.value = nn.Sequential(
            nn.Linear(head_hidden, head_hidden),
            nn.ReLU(inplace=True),
            nn.Linear(head_hidden, 1),
        )

    def forward(
        self, observation: Dict[str, torch.Tensor]
    ) -> Tuple[torch.Tensor, torch.Tensor]:
        board = observation["board"]
        scalars = observation["scalars"]
        candidates = observation["candidates"]
        mask = observation["mask"]

        spatial = self.trunk(self.stem(board))
        pooled = self.pool(spatial).flatten(1)
        if self.peak is not None:
            pooled = torch.cat([pooled, self.peak(spatial).flatten(1)], dim=1)
        embedded = self.embed(torch.cat([pooled, scalars], dim=1))

        count = candidates.shape[1]
        paired = torch.cat(
            [embedded.unsqueeze(1).expand(-1, count, -1), candidates], dim=2
        )
        logits = self.policy(paired).squeeze(-1)
        # A masked-out candidate must never be sampled, and must contribute
        # nothing to the entropy term either.
        logits = logits.masked_fill(mask <= 0, NEG_INF)

        return logits, self.value(embedded).squeeze(-1)

    @torch.no_grad()
    def act(
        self, observation: Dict[str, torch.Tensor], deterministic: bool = False
    ) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        logits, value = self.forward(observation)
        distribution = torch.distributions.Categorical(logits=logits)
        action = logits.argmax(dim=1) if deterministic else distribution.sample()
        return action, distribution.log_prob(action), value


def evaluate_actions(
    network: PolicyValueNet,
    observation: Dict[str, torch.Tensor],
    actions: torch.Tensor,
) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
    """Log-probabilities, entropy and values for a batch, for the PPO update."""
    logits, values = network(observation)
    distribution = torch.distributions.Categorical(logits=logits)
    return distribution.log_prob(actions), distribution.entropy(), values


def stack_observations(
    observations: List[Dict[str, np.ndarray]],
    device: Optional["torch.device"] = None,
) -> Dict[str, torch.Tensor]:
    """Batch environment observations into the tensors `forward` expects.

    `train.py` has its own copy pinned to the training device. This one lives
    here so that an agent built around a network -- a self-play opponent, say
    -- can run without importing the training module.
    """
    return {
        key: torch.as_tensor(
            np.stack([observation[key] for observation in observations])
        ).to(device=device, dtype=torch.float32)
        for key in ("board", "scalars", "candidates", "mask")
    }
