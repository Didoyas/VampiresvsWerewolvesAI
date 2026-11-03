"""Scoring a board from one species' point of view.

Positive is good for `species`. The score has no units; only comparisons
between sibling candidate turns matter.

Distance is plain Chebyshev. The board has no walls, so with eight-way
movement the number of steps between two cells is exactly
`max(|dx|, |dy|)` and no search is needed to find it.

The four terms, in rough order of importance:

  population   the win condition is simply having more creatures
  humans       a house we reach first is population we have not banked yet
  threat       creatures standing where the enemy can wipe them at 1.5x
  waste        groups too small to take any remaining house

Weights shift with the phase of the game. Early on, when houses are still on
the board, growth outweighs everything; once the humans are gone the only
thing left is the head count. The move cap matters too: with 200 shared turns
each side gets about 100, and a 50x50 board is 49 steps corner to corner, so a
house we cannot reach in time is worth nothing.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Tuple

from .rules import DEFAULT_UNKNOWNS, Unknowns, beats_deterministically
from .state import Coord, GameState, opponent_of

WIN = 1e9
LOSS = -1e9


def chebyshev(a: Coord, b: Coord) -> int:
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


@dataclass(frozen=True)
class Weights:
    population: float = 12.0
    humans: float = 6.0
    threat: float = 4.0
    waste: float = 1.5
    #: Extra credit for a house we could take outright right now.
    takeable_bonus: float = 0.6
    #: What a house is worth to a group currently too small to convert it.
    #: Zero on purpose. Any positive value pays the agent to hold extra
    #: groups, because each one "claims" another house it cannot take, and
    #: splitting then looks good even when it permanently ruins the stack:
    #: 11 creatures split 8/7 can never take a 19-house, while 11 kept whole
    #: grows 11 -> 15 -> 19 and takes two of them. Measured at 0.25 this cost
    #: 46 creatures of harvest across six maps.
    undersized_claim: float = 0.0
    #: How heavily to count a house we have no group free to contest.
    concession: float = 0.5
    #: An enemy two steps away needs a turn to close, so it threatens less.
    distant_threat_discount: float = 0.4


DEFAULT_WEIGHTS = Weights()


def turns_remaining(state: GameState, unknowns: Unknowns = DEFAULT_UNKNOWNS) -> int:
    """How many more turns this side can expect to play."""
    if unknowns.max_plays_counts_per_player:
        used = max(state.plies.values()) if state.plies else 0
        return max(0, unknowns.max_plays - used)
    used = sum(state.plies.values())
    return max(0, (unknowns.max_plays - used + 1) // 2)


def evaluate(
    state: GameState,
    species: str | None = None,
    weights: Weights = DEFAULT_WEIGHTS,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
) -> float:
    species = species or state.me
    enemy = opponent_of(species)

    mine = state.population(species)
    theirs = state.population(enemy)
    if mine == 0:
        return LOSS
    if theirs == 0:
        return WIN

    my_groups = state.groups(species)
    their_groups = state.groups(enemy)
    houses = state.humans()
    budget = turns_remaining(state, unknowns)

    # Phase: 1.0 while the board is mostly humans, 0.0 once they are gone.
    humans_left = sum(count for _, count in houses)
    board_total = mine + theirs + humans_left
    phase = humans_left / board_total if board_total else 0.0

    score = weights.population * (2.0 - phase) * (mine - theirs)
    score += weights.humans * phase * _human_potential(
        my_groups, their_groups, houses, budget, weights
    )
    score -= weights.threat * _threat(my_groups, their_groups, weights)
    score -= weights.waste * _wasted_groups(my_groups, houses)
    return score


def _human_potential(
    my_groups: List[Tuple[Coord, int]],
    their_groups: List[Tuple[Coord, int]],
    houses: List[Tuple[Coord, int]],
    budget: int,
    weights: Weights,
) -> float:
    """Houses we reach first, minus houses they reach first.

    Each house is assigned to at most one of our groups. Summing every
    house's pull instead would leave a group stranded between two of them:
    walking towards one walks away from another, the terms cancel, and the
    group dithers in place. One group, one target, so a step towards it is
    always an improvement. Assigning one house per group also prices in the
    fact that harvesting is parallel: a second house is only worth something
    if a second group can go and get it, which is what makes splitting
    attractive.

    Pairs are matched by value, richest *pairing* first, never richest house
    first. Walking the houses in descending size instead locks the one group
    we have onto the biggest house on the board even when it is far too small
    to convert it, and then concedes the houses it could actually have taken.
    That scored a whole-group conversion at -18.9 against +2.9 for pointlessly
    splitting, which is exactly backwards.
    """
    candidates = []
    for house_index, (house, humans) in enumerate(houses):
        their_distance = _closest(their_groups, house)
        for group_index, (at, count) in enumerate(my_groups):
            distance = chebyshev(at, house)
            if distance > budget or distance >= their_distance:
                continue  # out of reach, or they get there first
            value = humans * _reach(distance)
            value *= (
                1.0 + weights.takeable_bonus
                if count >= humans
                # This group would have to grow before the house is any use.
                # Without the discount a three-creature fragment "claims" a
                # 28-human house at full value, which pays the agent to keep
                # useless fragments instead of moving as one stack.
                else weights.undersized_claim
            )
            if value <= 0.0:
                continue  # a group that cannot take it does not cover it
            candidates.append((value, group_index, house_index))

    total = 0.0
    taken_groups, taken_houses = set(), set()
    for value, group_index, house_index in sorted(candidates, reverse=True):
        if group_index in taken_groups or house_index in taken_houses:
            continue
        taken_groups.add(group_index)
        taken_houses.add(house_index)
        total += value

    # Whatever we cannot cover, they are free to harvest.
    for house_index, (house, humans) in enumerate(houses):
        if house_index in taken_houses:
            continue
        their_distance = _closest(their_groups, house)
        if their_distance <= budget:
            total -= humans * _reach(their_distance) * weights.concession
    return total


def _reach(distance: int) -> float:
    """How much of a house's value survives having to walk to it.

    Geometric rather than 1/(1+d): every step closer is worth the same
    relative amount, so the gradient a planner follows never flattens out
    across a 50x50 board.
    """
    return 0.95**distance


def _closest(groups: List[Tuple[Coord, int]], target: Coord) -> int:
    if not groups:
        return 10**6
    return min(chebyshev(at, target) for at, _ in groups)


def _threat(
    my_groups: List[Tuple[Coord, int]],
    their_groups: List[Tuple[Coord, int]],
    weights: Weights,
) -> float:
    """Creatures of ours the enemy could wipe out by reaching the 1.5x bar.

    Only groups the enemy can actually overpower count: being next to a group
    that cannot beat us is not a threat, it is an opportunity.
    """
    exposed = 0.0
    for at, count in my_groups:
        for enemy_at, enemy_count in their_groups:
            if not beats_deterministically(enemy_count, count):
                continue
            distance = chebyshev(at, enemy_at)
            if distance <= 1:
                exposed += count
                break
            if distance == 2:
                exposed += count * weights.distant_threat_discount
                break
    return exposed


def _wasted_groups(
    my_groups: List[Tuple[Coord, int]], houses: List[Tuple[Coord, int]]
) -> float:
    """Groups that cannot do any useful work.

    Splitting is usually good here, because houses can be harvested in
    parallel, so while houses remain this only counts fragments too small to
    take the cheapest one. Once the houses are gone the calculus inverts:
    nothing is left to harvest, and a detached group can only be caught alone
    and killed, so the population wants to be in one stack.

    Measured in *groups*, not creatures. Charging per creature was tried and
    cost 13 creatures of average margin against the naive agent: it made the
    penalty large enough to suppress splitting altogether, and harvesting
    houses one at a time loses the race inside the move cap.
    """
    if not my_groups:
        return 0.0
    if not houses:
        # Charging per creature here instead of per group was tried, to push
        # the endgame towards massing for the 1.5x bar, and measured as noise
        # (-0.2 margin against both baselines). The term is too small next to
        # the population term to drive a multi-turn regrouping march, so the
        # simpler count stays. See docs/paper_notes.md for the diagnosis of
        # the one map where this costs real creatures.
        return float(len(my_groups) - 1)
    # While houses remain, count groups rather than creatures. Charging per
    # creature here suppresses splitting altogether and costs 13% of harvest.
    smallest = min(count for _, count in houses)
    return float(sum(1 for _, count in my_groups if count < smallest))
