"""Turning a board into network input.

Two pieces come out of here:

  planes   a (C, H, W) float array, the spatial input to a convolutional
           trunk, padded up to MAX_SIDE so a batch can mix board sizes
  scalars  a small vector of whole-board quantities

The scalars are not a convenience. A convolutional trunk with global pooling
throws away absolute totals, and the win condition in this game is nothing
but an absolute total: who has more creatures. Without them the network can
see the shape of a position but not who is winning it.

Everything is written from the moving side's point of view — our creatures
are always plane 0 — so one network plays both species, which the rules
allow because vampires and werewolves are identical apart from the name.
"""

from __future__ import annotations

import math
from typing import Tuple

import numpy as np

from .evaluate import turns_remaining
from .mapgen import MAX_SIDE
from .rules import DEFAULT_UNKNOWNS, Unknowns
from .state import GameState, opponent_of, species_index

#: ours, theirs, humans, house marker, on-board marker
PLANES = 5
SCALARS = 5

#: Counts travel as one byte, so this is the largest value any cell can hold.
_MAX_COUNT = 255.0
_LOG_MAX = math.log1p(_MAX_COUNT)


def _squash(count: float) -> float:
    """Compress a population into [0, 1].

    Logarithmic because the difference between 3 and 8 creatures decides
    whether a house can be taken, while the difference between 200 and 205
    decides nothing.
    """
    return math.log1p(max(0.0, count)) / _LOG_MAX


def encode_planes(
    state: GameState, species: str | None = None, size: int = MAX_SIDE
) -> np.ndarray:
    """Spatial input, padded to (PLANES, size, size) with the board at (0, 0)."""
    species = species or state.me
    mine = species_index(species)
    theirs = species_index(opponent_of(species))

    planes = np.zeros((PLANES, size, size), dtype=np.float32)
    # Plane 4 marks the real board, so padding is distinguishable from empty
    # cells; without it a 10x10 board looks like a 50x50 board that is mostly
    # deserted, and "deserted" is a very different position.
    planes[4, : state.rows, : state.columns] = 1.0

    for (x, y), contents in state.cells.items():
        if not (0 <= y < size and 0 <= x < size):
            continue
        planes[0, y, x] = _squash(contents[mine])
        planes[1, y, x] = _squash(contents[theirs])
        planes[2, y, x] = _squash(contents[0])

    for x, y in state.houses:
        if 0 <= y < size and 0 <= x < size:
            planes[3, y, x] = 1.0

    return planes


def encode_scalars(
    state: GameState,
    species: str | None = None,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
) -> np.ndarray:
    """Whole-board quantities that pooling would otherwise destroy."""
    species = species or state.me
    mine = state.population(species)
    theirs = state.population(opponent_of(species))
    humans = state.human_population()
    total = max(1, mine + theirs)

    return np.array(
        [
            _squash(mine),
            _squash(theirs),
            _squash(humans),
            # Who is ahead, in [-1, 1]. This is the win condition itself.
            (mine - theirs) / total,
            turns_remaining(state, unknowns) / max(1, unknowns.max_plays),
        ],
        dtype=np.float32,
    )


def encode(
    state: GameState, species: str | None = None, size: int = MAX_SIDE
) -> Tuple[np.ndarray, np.ndarray]:
    return encode_planes(state, species, size), encode_scalars(state, species)


#: Per-candidate features; see `candidate_features`.
CANDIDATE_FEATURES = 10


def candidate_features(
    before: GameState,
    after: GameState,
    turn,
    species: str,
    score: float,
    rank: float = 0.0,
) -> np.ndarray:
    """Describe what one candidate turn actually does.

    Without this the policy sees only the board, so every candidate looks
    alike and the only thing it can learn is how far down the heuristic's
    ranking to reach. Given what each turn leads to, it can judge them on
    their merits and overrule that ranking where the ranking is wrong.

    The first seven features describe outcomes, and on their own they were
    not enough: measured across 180 states, **81.7% had at least two
    candidates with identical feature vectors**. Identical features mean
    identical logits, so the policy cannot prefer one over the other, and
    worse, when the heuristic's pick has a twin the supervised target is
    unlearnable noise -- which is why warm-start accuracy plateaued near 0.5.

    Most ties were turns that differ only in *where* they go: step a group
    left or right onto empty ground and every outcome count is the same. The
    last three features are therefore spatial -- progress towards a house we
    can afford, change in distance to the enemy -- plus the heuristic's rank,
    which is distinct by construction and so breaks any remaining tie.
    """
    them = opponent_of(species)
    mine_before = before.population(species)
    mine_after = after.population(species)
    theirs_before = before.population(them)
    theirs_after = after.population(them)
    scale = max(1, mine_before + theirs_before)

    groups_after = after.groups(species)
    biggest = max((count for _, count in groups_after), default=0)

    return np.array(
        [
            (mine_after - mine_before) / scale,  # how much we grew
            (theirs_before - theirs_after) / scale,  # how much we killed
            (before.human_population() - after.human_population()) / scale,
            min(1.0, len(turn) / 8.0),  # how wide the turn is
            min(1.0, len(groups_after) / 8.0),  # how split we end up
            biggest / max(1, mine_after),  # how concentrated the force stays
            float(np.tanh(score / 1000.0)),  # the heuristic's own opinion
            _house_progress(before, after, species),
            _enemy_distance_change(before, after, species),
            rank,  # distinct per candidate, so no two can ever tie
        ],
        dtype=np.float32,
    )


def _nearest_affordable_house(state: GameState, species: str) -> float:
    """Steps from our closest group to a house it could convert outright."""
    from .evaluate import chebyshev

    groups = state.groups(species)
    best = None
    for cell, humans in state.humans():
        for at, count in groups:
            if count < humans:
                continue
            distance = chebyshev(at, cell)
            if best is None or distance < best:
                best = distance
    return float(best) if best is not None else -1.0


def _house_progress(before: GameState, after: GameState, species: str) -> float:
    """Did this turn get us closer to a house we can take? In [-1, 1]."""
    start = _nearest_affordable_house(before, species)
    end = _nearest_affordable_house(after, species)
    if start < 0 or end < 0:
        return 0.0
    return float(np.clip((start - end) / 4.0, -1.0, 1.0))


def _enemy_distance_change(before: GameState, after: GameState, species: str) -> float:
    """Positive when the turn closes on the enemy, negative when it backs off."""
    from .evaluate import chebyshev

    enemy = opponent_of(species)

    def closest(state: GameState) -> float:
        mine = state.groups(species)
        theirs = state.groups(enemy)
        if not mine or not theirs:
            return -1.0
        return float(
            min(chebyshev(a, b) for a, _ in mine for b, _ in theirs)
        )

    start, end = closest(before), closest(after)
    if start < 0 or end < 0:
        return 0.0
    return float(np.clip((start - end) / 4.0, -1.0, 1.0))
