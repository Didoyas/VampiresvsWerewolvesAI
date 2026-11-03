"""An agent that plays by asking a trained network.

This is what makes self-play possible. The opponent pool holds callables of
the shape `(state, species) -> turn`; this wraps a `PolicyValueNet` into that
shape, so a snapshot of the learner can sit in the pool beside the heuristic
and the naive agent.

The opponent sees exactly what the learner sees -- same candidate generator,
same shuffle, same features -- because both go through `env.observation_for`.
Anything else would be training against a different game.

Torch is imported inside the methods on purpose: `main.py` is the tournament
client and must keep running on a machine where torch is not installed, and
it reaches the agents package.
"""

from __future__ import annotations

import random
from typing import Any, Dict, List, Optional

from ..rules import DEFAULT_UNKNOWNS, Move, Unknowns
from ..state import GameState


class PolicyAgent:
    """Picks among the generated candidates with a policy network."""

    def __init__(
        self,
        network: Any,
        unknowns: Unknowns = DEFAULT_UNKNOWNS,
        deterministic: bool = False,
        seed: Optional[int] = None,
    ):
        #: Sampling rather than argmax by default. A deterministic sparring
        #: partner plays one line and the learner overfits to it; the pool
        #: exists to be varied, so the snapshots stay stochastic.
        self.network = network
        self.unknowns = unknowns
        self.deterministic = deterministic
        self._rng = random.Random(seed)

    @classmethod
    def from_snapshot(
        cls,
        state_dict: Dict[str, Any],
        channels: int,
        blocks: int,
        head_hidden: int,
        **kwargs: Any,
    ) -> "PolicyAgent":
        """Rebuild an agent from weights alone.

        Snapshots travel to the worker processes as plain state dicts, kept
        on the CPU: the workers have no GPU context of their own, and a
        78k-parameter network costs about 300 KB to send.
        """
        from ..network import PolicyValueNet

        network = PolicyValueNet(
            channels=channels, blocks=blocks, head_hidden=head_hidden
        )
        network.load_state_dict(state_dict)
        network.eval()
        return cls(network, **kwargs)

    def __call__(self, state: GameState, species: str) -> List[Move]:
        import torch

        from ..candidates import observation_for
        from ..network import stack_observations

        observation, turns = observation_for(
            state, species, self.unknowns, self._rng
        )
        if not turns:
            return []
        with torch.no_grad():
            action, _, _ = self.network.act(
                stack_observations([observation]), deterministic=self.deterministic
            )
        return turns[min(int(action[0]), len(turns) - 1)]


def choose_turn(state: GameState, species: str) -> List[Move]:
    """Not provided: this agent needs a network, so build a `PolicyAgent`.

    The other agents in this package expose a module-level `choose_turn`, and
    this exists so that reaching for the same name fails loudly instead of
    silently importing something that cannot work.
    """
    raise TypeError(
        "agents.policy has no module-level agent; construct PolicyAgent(network)"
    )
