"""An agent that moves at random, but always legally.

Two jobs. It is the floor any real agent must clear by a wide margin, and it
is the exploration end of the opponent pool in training: a learner that only
ever meets competent opponents never sees the strange positions it will be
punished for mishandling later.

It is deliberately not "random among sensible moves" — it may walk into a
fight it loses. That is the point of a floor.
"""

from __future__ import annotations

import random
from typing import List, Optional, Set

from ..rules import Move
from ..state import Coord, GameState, species_index

#: Chance a given group simply holds position, so turns vary in width.
HOLD_PROBABILITY = 0.3

_RNG = random.Random()


def seed(value: int) -> None:
    """Make a series of games reproducible."""
    _RNG.seed(value)


def choose_turn(
    state: GameState,
    species: Optional[str] = None,
    rng: Optional[random.Random] = None,
) -> List[Move]:
    species = species or state.me
    rng = rng or _RNG
    idx = species_index(species)

    groups = state.groups(species)
    rng.shuffle(groups)

    moves: List[Move] = []
    sources: Set[Coord] = set()
    targets: Set[Coord] = set()

    for coord, count in groups:
        if coord in targets:  # rule 5: something is already arriving here
            continue
        options = [n for n in state.neighbours(coord) if n not in sources]
        if not options or rng.random() < HOLD_PROBABILITY:
            continue
        destination = rng.choice(options)
        moving = rng.randint(1, count)
        moves.append(Move(coord[0], coord[1], moving, *destination))
        sources.add(coord)
        targets.add(destination)

    if not moves:
        moves = _forced_move(state, species, idx, rng)
    return moves


def _forced_move(
    state: GameState, species: str, idx: int, rng: random.Random
) -> List[Move]:
    """Rule 1 still applies on a turn where every group chose to hold."""
    for coord, count in state.groups(species):
        options = list(state.neighbours(coord))
        if options:
            return [Move(coord[0], coord[1], count, *rng.choice(options))]
    return []
