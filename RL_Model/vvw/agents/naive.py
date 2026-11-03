"""A small always-legal agent.

This is P0's opponent-shaped placeholder and, just as importantly, the
guaranteed fallback: whenever search or inference fails or runs out of time,
the tournament client sends what `choose_turn` returns here.  It is deliberately
simple.  The real heuristic agent arrives in P2.

What it does: walk each group towards the nearest house it can take outright,
take a house or an enemy group when standing next to one and the odds are
certain, and never step into a cell where it would lose a fight it could have
avoided.
"""

from __future__ import annotations

from typing import List, Optional, Set

from ..rules import (
    Move,
    beats_deterministically,
    converts_deterministically,
)
from ..state import (
    Coord,
    GameState,
    HUMANS_IDX,
    opponent_of,
    species_index,
)


def chebyshev(a: Coord, b: Coord) -> int:
    """Steps needed with eight-way movement."""
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def choose_turn(state: GameState, species: Optional[str] = None) -> List[Move]:
    """Pick a legal turn.  Never returns an empty list on a non-empty board."""
    species = species or state.me
    mine = species_index(species)
    theirs = species_index(opponent_of(species))

    groups = sorted(state.groups(species), key=lambda g: -g[1])
    moves: List[Move] = []
    sources: Set[Coord] = set()
    targets: Set[Coord] = set()

    for coord, count in groups:
        if coord in targets:  # rule 5: it is already somebody's destination
            continue
        dest = _best_step(state, coord, count, mine, theirs, sources)
        if dest is None:
            continue
        moves.append(Move(coord[0], coord[1], count, dest[0], dest[1]))
        sources.add(coord)
        targets.add(dest)

    if not moves:
        fallback = _any_legal_move(state, species, mine, theirs)
        if fallback is not None:
            moves.append(fallback)
    return moves


def _best_step(
    state: GameState,
    coord: Coord,
    count: int,
    mine: int,
    theirs: int,
    sources: Set[Coord],
) -> Optional[Coord]:
    """The most useful of the eight neighbours, or None to stay put."""
    goal = _nearest_goal(state, coord, count, mine, theirs)
    best, best_score = None, None

    for neighbour in state.neighbours(coord):
        if neighbour in sources:  # rule 5
            continue
        score = _score_step(state, neighbour, count, mine, theirs)
        if score is None:  # stepping here would lose a fight
            continue
        if goal is not None:
            # prefer steps that close the distance to the goal
            score += (chebyshev(coord, goal) - chebyshev(neighbour, goal)) * 10
        if best_score is None or score > best_score:
            best, best_score = neighbour, score

    return best


def _score_step(
    state: GameState, cell: Coord, count: int, mine: int, theirs: int
) -> Optional[float]:
    """Value of stepping onto `cell`, or None if it is a losing fight."""
    humans, vampires, werewolves = state.get(cell)
    occupants = (humans, vampires, werewolves)
    enemy = occupants[theirs]
    friend = occupants[mine]

    if humans > 0:
        if converts_deterministically(count, humans):
            return 100.0 + humans  # free growth
        return None  # a coin flip we do not need to take
    if enemy > 0:
        if beats_deterministically(count, enemy):
            return 80.0 + enemy
        return None
    if friend > 0:
        return 5.0  # merging is mildly useful
    return 1.0


def _nearest_goal(
    state: GameState, coord: Coord, count: int, mine: int, theirs: int
) -> Optional[Coord]:
    """Closest house we could take outright, else the closest beatable group."""
    best, best_key = None, None
    for cell, humans in state.humans():
        if not converts_deterministically(count, humans):
            continue
        key = (chebyshev(coord, cell), -humans)
        if best_key is None or key < best_key:
            best, best_key = cell, key
    if best is not None:
        return best

    for cell, contents in state.cells.items():
        enemy = contents[theirs]
        if enemy <= 0 or not beats_deterministically(count, enemy):
            continue
        key = (chebyshev(coord, cell), -enemy)
        if best_key is None or key < best_key:
            best, best_key = cell, key
    return best


def _any_legal_move(
    state: GameState, species: str, mine: int, theirs: int
) -> Optional[Move]:
    """Last resort: one creature, one step, preferring a harmless cell.

    Only returns a losing move if literally every neighbour of every group is
    a losing move, in which case the turn has to happen anyway (rule 1).
    """
    groups = sorted(state.groups(species), key=lambda g: -g[1])
    worst_case: Optional[Move] = None

    for coord, count in groups:
        for neighbour in state.neighbours(coord):
            move = Move(coord[0], coord[1], count, neighbour[0], neighbour[1])
            if _score_step(state, neighbour, count, mine, theirs) is not None:
                return move
            if worst_case is None:
                worst_case = move
    return worst_case
