"""Applying a turn to a state.

This is the single rules core.  Training, search and the local match runner all
call `apply_turn`, so there is no second implementation that could drift away
from this one.  `tools/probe_server.py` checks it against the real server.

Order of resolution within one turn:
  1. every mover leaves its source,
  2. movers heading for the same cell merge (see
     Unknowns.moves_into_one_target_merge_first),
  3. each destination resolves once, against humans or enemies or nobody.

Rule 5 guarantees a destination never also sent creatures away this turn, so
the three steps cannot interfere with each other.
"""

from __future__ import annotations

import random
from typing import Dict, List

from .rules import (
    DEFAULT_UNKNOWNS,
    Move,
    Turn,
    Unknowns,
    likeliest_outcome_vs_creatures,
    likeliest_outcome_vs_humans,
    resolve_vs_creatures,
    resolve_vs_humans,
    validate_turn,
)
from .state import (
    Coord,
    GameState,
    HUMANS_IDX,
    opponent_of,
    species_index,
)


def apply_turn(
    state: GameState,
    turn: Turn,
    species: str | None = None,
    rng: random.Random | None = None,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
    validate: bool = True,
    expected: bool = False,
) -> GameState:
    """Return a new state with `turn` played by `species`.

    `rng` is only consulted if some destination triggers a random battle.

    With `expected=True` no dice are rolled: a random battle collapses to its
    likelier side, with that side's conditional expected survivors. Planners
    use this to score a candidate turn without sampling. It is an
    approximation, so it is never used to advance the real game; the agent
    also avoids low-probability battles in the first place.
    """
    species = species or state.me
    if validate:
        problems = validate_turn(state, turn, species, unknowns)
        if problems:
            raise ValueError("illegal turn: " + "; ".join(problems))

    nxt = state.copy()
    mine = species_index(species)
    theirs = species_index(opponent_of(species))

    # 1. leave the sources
    for move in turn:
        contents = list(nxt.get(move.source))
        contents[mine] -= move.count
        nxt.set_cell(move.source, tuple(contents))

    # 2. merge arrivals per destination
    arriving: Dict[Coord, int] = {}
    for move in turn:
        arriving[move.target] = arriving.get(move.target, 0) + move.count

    # 3. resolve each destination
    for target, incoming in arriving.items():
        humans, vampires, werewolves = nxt.get(target)
        occupants = [humans, vampires, werewolves]
        defenders = occupants[theirs]
        friendly = occupants[mine]

        if humans > 0:
            outcome = (
                likeliest_outcome_vs_humans(incoming, humans, unknowns)
                if expected
                else resolve_vs_humans(incoming, humans, rng, unknowns)
            )
            occupants[HUMANS_IDX] = outcome.defenders
            occupants[mine] = friendly + outcome.attackers + outcome.converted
        elif defenders > 0:
            outcome = (
                likeliest_outcome_vs_creatures(incoming, defenders)
                if expected
                else resolve_vs_creatures(incoming, defenders, rng, unknowns)
            )
            occupants[theirs] = outcome.defenders
            occupants[mine] = friendly + outcome.attackers
        else:
            # empty cell, or one of ours already standing there: merge
            occupants[mine] = friendly + incoming

        nxt.set_cell(target, tuple(occupants))

    nxt.plies[species] = nxt.plies.get(species, 0) + 1
    return nxt


def is_terminal(state: GameState, unknowns: Unknowns = DEFAULT_UNKNOWNS) -> bool:
    """A species was wiped out, or the move cap was reached."""
    from .state import VAMPIRES, WEREWOLVES

    if state.population(VAMPIRES) == 0 or state.population(WEREWOLVES) == 0:
        return True
    if unknowns.max_plays_counts_per_player:
        return max(state.plies.values()) >= unknowns.max_plays
    return sum(state.plies.values()) >= unknowns.max_plays


def winner(
    state: GameState,
    first_mover: str | None = None,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
) -> str | None:
    """Who wins the finished game, or None for a draw.

    At either cap -- 200 shared moves or five minutes -- the creatures on the
    board are compared and humans count for nothing. A win needs strictly
    more; equal counts are a real draw, which a competitor can play for.

    `first_mover` exists only for the retired tie-break in
    `Unknowns.tie_at_cap_loses_for_first_mover`, which now defaults off.
    """
    from .state import VAMPIRES, WEREWOLVES

    v = state.population(VAMPIRES)
    w = state.population(WEREWOLVES)
    if v > w:
        return VAMPIRES
    if w > v:
        return WEREWOLVES
    if (
        unknowns.tie_at_cap_loses_for_first_mover
        and first_mover is not None
        and (v or w)  # both sides still alive, so the cap is what ended it
    ):
        return opponent_of(first_mover)
    return None
