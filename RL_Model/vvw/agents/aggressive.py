"""An agent that hunts the enemy instead of farming quietly.

It exists because the other agents never fight each other. On a symmetric map
two harvesters split the humans down the middle, finish level, and the combat
rules are never exercised at all — so neither a benchmark nor a training run
built only on harvesters says anything about how an agent handles being
attacked.

Its policy: grow only as much as it needs to, then close on the enemy and
take fights it is certain to win. It is not meant to be the strongest agent,
it is meant to be an awkward one.
"""

from __future__ import annotations

from typing import List, Optional, Set

from ..evaluate import chebyshev
from ..rules import Move, beats_deterministically, converts_deterministically
from ..state import Coord, GameState, opponent_of, species_index


def choose_turn(state: GameState, species: Optional[str] = None) -> List[Move]:
    species = species or state.me
    mine = species_index(species)
    theirs = species_index(opponent_of(species))

    enemies = [
        (cell, contents[theirs])
        for cell, contents in state.cells.items()
        if contents[theirs] > 0
    ]
    groups = sorted(state.groups(species), key=lambda g: -g[1])

    moves: List[Move] = []
    sources: Set[Coord] = set()
    targets: Set[Coord] = set()

    for coord, count in groups:
        if coord in targets:
            continue
        destination = _choose_destination(
            state, coord, count, mine, theirs, enemies, sources
        )
        if destination is None:
            continue
        moves.append(Move(coord[0], coord[1], count, *destination))
        sources.add(coord)
        targets.add(destination)

    if not moves:
        from . import naive

        return naive.choose_turn(state, species)
    return moves


def _choose_destination(
    state: GameState,
    coord: Coord,
    count: int,
    mine: int,
    theirs: int,
    enemies,
    sources: Set[Coord],
) -> Optional[Coord]:
    options = [cell for cell in state.neighbours(coord) if cell not in sources]
    if not options:
        return None

    # Kill something next door if we are certain to win.
    for cell in options:
        enemy = state.get(cell)[theirs]
        if enemy > 0 and beats_deterministically(count, enemy):
            return cell

    # Otherwise feed: take a house we outnumber.
    for cell in options:
        humans = state.get(cell)[0]
        if humans > 0 and converts_deterministically(count, humans):
            return cell

    goal = _hunting_ground(state, coord, count, enemies)
    if goal is None:
        return None

    best, best_distance = None, chebyshev(coord, goal)
    for cell in options:
        if _suicidal(state, cell, count, theirs):
            continue
        distance = chebyshev(cell, goal)
        if distance < best_distance:
            best, best_distance = cell, distance
    return best


def _hunting_ground(
    state: GameState, coord: Coord, count: int, enemies
) -> Optional[Coord]:
    """The enemy group we could beat, else the house that would let us."""
    beatable = [
        (chebyshev(coord, cell), cell)
        for cell, enemy in enemies
        if beats_deterministically(count, enemy)
    ]
    if beatable:
        return min(beatable)[1]

    affordable = [
        (chebyshev(coord, cell), cell)
        for cell, humans in state.humans()
        if converts_deterministically(count, humans) and chebyshev(coord, cell) > 0
    ]
    if affordable:
        return min(affordable)[1]
    return None


def _suicidal(state: GameState, cell: Coord, count: int, theirs: int) -> bool:
    contents = state.get(cell)
    if contents[0] > 0 and not converts_deterministically(count, contents[0]):
        return True
    enemy = contents[theirs]
    return enemy > 0 and not beats_deterministically(count, enemy)
