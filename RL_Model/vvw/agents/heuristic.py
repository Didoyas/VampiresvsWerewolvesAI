"""The baseline agent: intent shortlists assembled into whole turns by beam.

A turn here is a *compound* action — any subset of our groups may move, each
wholly or in part — so the space of legal turns is far too large to enumerate.
Instead each group proposes a handful of tactically meaningful intents, and a
beam stitches them into complete turns. Nothing is ever enumerated blindly,
and the archetypes below cover the moves that decide games:

    convert    step onto a house we outnumber, converting it for free
    split      divide a group so two houses fall in the same turn
    attack     step onto enemies we beat at the 1.5x bar
    regroup    merge toward a friendly group to clear that bar next turn
    retreat    step away from an enemy who could wipe us out
    advance    close on the best house we are nearer to than they are

Only the completed turns are scored properly, by applying them and calling
`evaluate`. The partial scores used during assembly are a cheap ordering key,
not a judgement.

With roughly 100 turns a side and up to 49 steps to cross a board, splitting to
harvest houses in parallel is usually worth more than marching one big stack
around, which is why the split intents exist.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, FrozenSet, List, Optional, Sequence, Tuple

from ..evaluate import (
    DEFAULT_WEIGHTS,
    Weights,
    chebyshev,
    evaluate,
    turns_remaining,
)
from ..rules import (
    DEFAULT_UNKNOWNS,
    Move,
    Unknowns,
    beats_deterministically,
    converts_deterministically,
    validate_turn,
    win_probability,
)
from ..simulator import apply_turn
from ..state import Coord, GameState, opponent_of, species_index

#: Default floor on a random battle's win probability, when the game is
#: level. `battle_threshold` moves it with the score; see there for why.
MIN_BATTLE_ODDS = 0.65
#: The lowest odds worth taking even when losing is otherwise certain.
DESPERATE_BATTLE_ODDS = 0.15

#: Candidate turns kept while assembling, and intents offered per group.
DEFAULT_BEAM = 8
MAX_INTENTS_PER_GROUP = 6


@dataclass(frozen=True)
class Plan:
    """A partially assembled turn, carrying what rules 3 and 5 need."""

    moves: Tuple[Move, ...] = ()
    sources: FrozenSet[Coord] = frozenset()
    targets: FrozenSet[Coord] = frozenset()
    spent: Tuple[Tuple[Coord, int], ...] = ()
    score: float = 0.0

    def sent_from(self, coord: Coord) -> int:
        return dict(self.spent).get(coord, 0)

    def extend(self, moves: Sequence[Move], value: float) -> "Plan":
        spent = dict(self.spent)
        for move in moves:
            spent[move.source] = spent.get(move.source, 0) + move.count
        return Plan(
            moves=self.moves + tuple(moves),
            sources=self.sources | {m.source for m in moves},
            targets=self.targets | {m.target for m in moves},
            spent=tuple(sorted(spent.items())),
            score=self.score + value,
        )

    def accepts(self, state: GameState, moves: Sequence[Move], idx: int) -> bool:
        """Would appending these moves keep the whole turn legal?"""
        new_sources = {m.source for m in moves}
        new_targets = {m.target for m in moves}
        # Rule 5, across the turn as a whole.
        if new_sources & (self.targets | new_targets):
            return False
        if new_targets & self.sources:
            return False
        # Rule 3, counting everything already leaving each source.
        outgoing: Dict[Coord, int] = {}
        for move in moves:
            outgoing[move.source] = outgoing.get(move.source, 0) + move.count
        for coord, leaving in outgoing.items():
            available = state.get(coord)[idx]
            if self.sent_from(coord) + leaving > available:
                return False
        return True


def battle_threshold(
    state: GameState, species: str, unknowns: Unknowns = DEFAULT_UNKNOWNS
) -> float:
    """How good a random battle has to look before it is worth taking.

    A fixed floor is wrong at both ends of the score. Ahead, a coin-flip
    risks a won game for nothing. Behind, refusing every gamble guarantees
    the loss: equal counts are a draw and a draw is not a win, so a side that
    is behind with no affordable house left and no path to the 1.5x bar has
    exactly one way to win, and it is the fight it keeps declining.

    Concretely, 12 against 15 with nothing left to harvest is a dead loss if
    both sides sit still, and a 40% chance of taking the game outright if we
    attack -- because winning that battle wipes them out entirely. The floor
    therefore falls as our position worsens and as the clock runs down, and
    rises when we are ahead and have something to protect.
    """
    mine = state.population(species)
    theirs = state.population(opponent_of(species))
    if mine == 0 or theirs == 0:
        return MIN_BATTLE_ODDS

    if mine > theirs:
        return 0.8  # winning: a gamble can only cost us the win

    budget = turns_remaining(state, unknowns)
    urgency = 1.0 - min(1.0, budget / max(1, unknowns.max_plays))
    if mine == theirs:
        # Level is a draw, which is worth something but is not a win.
        return max(0.5, MIN_BATTLE_ODDS - 0.15 * urgency)

    # Behind. If no house can still be afforded, harvesting will not save us.
    affordable = any(
        count >= humans
        for _, count in state.groups(species)
        for _, humans in state.humans()
    )
    floor = DESPERATE_BATTLE_ODDS if not affordable else 0.4
    return max(floor, MIN_BATTLE_ODDS - (MIN_BATTLE_ODDS - floor) * urgency)


def choose_turn(
    state: GameState,
    species: Optional[str] = None,
    beam: int = DEFAULT_BEAM,
    weights: Weights = DEFAULT_WEIGHTS,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
) -> List[Move]:
    """Pick a legal turn. Never returns empty while we still have creatures."""
    species = species or state.me
    idx = species_index(species)
    groups = sorted(state.groups(species), key=lambda g: -g[1])
    if not groups:
        return []
    odds_floor = battle_threshold(state, species, unknowns)

    plans: List[Plan] = [Plan()]
    for coord, count in groups:
        extended: List[Plan] = []
        for plan in plans:
            extended.append(plan)  # this group may simply hold position
            if coord in plan.targets:
                continue  # rule 5: someone is already moving in here
            for moves, value in _intents(
                state, coord, count, species, idx, plan, odds_floor
            ):
                if plan.accepts(state, moves, idx):
                    extended.append(plan.extend(moves, value))
        plans = sorted(extended, key=lambda p: -p.score)[: max(1, beam)]

    return _pick_best(state, plans, species, weights, unknowns)


def _pick_best(
    state: GameState,
    plans: List[Plan],
    species: str,
    weights: Weights,
    unknowns: Unknowns,
) -> List[Move]:
    """Score completed turns properly and return the best legal one."""
    best_moves: List[Move] = []
    best_score = None

    for plan in plans:
        if not plan.moves:
            continue
        moves = list(plan.moves)
        if validate_turn(state, moves, species, unknowns):
            continue
        after = apply_turn(
            state, moves, species, unknowns=unknowns, validate=False, expected=True
        )
        score = evaluate(after, species, weights, unknowns)
        if best_score is None or score > best_score:
            best_moves, best_score = moves, score

    if best_moves:
        return best_moves
    # Everything was filtered out; fall back to something certainly legal.
    from . import naive

    return naive.choose_turn(state, species)


# ---------------------------------------------------------------------------
# Intents
# ---------------------------------------------------------------------------


def _intents(
    state: GameState,
    coord: Coord,
    count: int,
    species: str,
    idx: int,
    plan: Plan,
    odds_floor: float = MIN_BATTLE_ODDS,
) -> List[Tuple[List[Move], float]]:
    """A short list of tactically meaningful options for one group."""
    enemy_idx = species_index(opponent_of(species))
    available = count - plan.sent_from(coord)
    if available <= 0:
        return []

    options: List[Tuple[List[Move], float]] = []
    neighbours = list(state.neighbours(coord))

    houses_next_door = [
        (cell, state.get(cell)[0]) for cell in neighbours if state.get(cell)[0] > 0
    ]
    enemies_next_door = [
        (cell, state.get(cell)[enemy_idx])
        for cell in neighbours
        if state.get(cell)[enemy_idx] > 0
    ]

    # Convert a neighbouring house. Taking the whole group is the default:
    # sending only the creatures the house needs looks thriftier, but the
    # remainder is usually left behind with no job, and a stray fragment is
    # worth less than the stack it came from. The thrifty variant is still
    # offered, and wins when the leftovers have somewhere else to be.
    for cell, humans in houses_next_door:
        if converts_deterministically(available, humans):
            options.append(([Move(*coord, available, *cell)], 20.0 * humans))
            if humans < available:
                options.append(([Move(*coord, humans, *cell)], 18.0 * humans))

    # Take two neighbouring houses at once.
    if len(houses_next_door) >= 2:
        first, second = sorted(houses_next_door, key=lambda h: -h[1])[:2]
        if first[1] + second[1] <= available:
            options.append(
                (
                    [
                        Move(*coord, first[1], *first[0]),
                        Move(*coord, second[1], *second[0]),
                    ],
                    22.0 * (first[1] + second[1]),
                )
            )

    # Clear out a neighbouring enemy we are certain to beat.
    for cell, enemy_count in enemies_next_door:
        needed = -(-3 * enemy_count // 2)  # ceil(1.5 * enemy)
        if needed <= available:
            options.append(([Move(*coord, needed, *cell)], 15.0 * enemy_count))
        elif beats_deterministically(available, enemy_count):
            options.append(([Move(*coord, available, *cell)], 15.0 * enemy_count))
        elif win_probability(available, enemy_count) >= odds_floor:
            odds = win_probability(available, enemy_count)
            options.append(([Move(*coord, available, *cell)], 8.0 * enemy_count * odds))

    # Walk towards whatever is worth reaching, or away from danger.
    step = _advance_step(state, coord, available, species, idx, enemy_idx, odds_floor)
    if step is not None:
        destination, value = step
        options.append(([Move(*coord, available, *destination)], value))

    retreat = _retreat_step(
        state, coord, available, species, idx, enemy_idx, odds_floor
    )
    if retreat is not None:
        options.append(([Move(*coord, available, *retreat)], 12.0))

    divide = _split_towards_two_houses(
        state, coord, available, species, idx, enemy_idx, odds_floor
    )
    if divide is not None:
        options.append(divide)

    regroup = _regroup_step(
        state, coord, available, species, idx, enemy_idx, odds_floor
    )
    if regroup is not None:
        destination, value = regroup
        options.append(([Move(*coord, available, *destination)], value))

    options.sort(key=lambda o: -o[1])
    return options[:MAX_INTENTS_PER_GROUP]


def _advance_step(
    state: GameState,
    coord: Coord,
    count: int,
    species: str,
    idx: int,
    enemy_idx: int,
    odds_floor: float = MIN_BATTLE_ODDS,
) -> Optional[Tuple[Coord, float]]:
    """One step towards the most attractive house, if any is worth chasing."""
    # Houses we can actually convert come first, and only if there are none
    # do we consider the ones we would have to grow into. Ranking them
    # together lets a big unreachable house outscore a modest reachable one,
    # and the group then walks up next to it and stops: it cannot step on,
    # because that is a losing fight, and nothing else is closer. That stall
    # froze the agent at its starting population for whole games.
    #
    # A house we cannot take is also no use to stand next to, so those are
    # dropped once adjacent, otherwise the group arrives and has nowhere to go.
    reachable, aspirational = [], []
    for cell, humans in state.humans():
        distance = chebyshev(coord, cell)
        if distance == 0:
            continue
        # Same geometric decay the evaluator uses, so the intent that gets
        # proposed and the score that judges it agree on where to go.
        value = humans * (0.95**distance)
        if count >= humans:
            reachable.append((value, cell))
        elif distance > 1:
            aspirational.append((value * 0.25, cell))

    candidates = reachable or aspirational
    goal, goal_value = None, 0.0
    if candidates:
        goal_value, goal = max(candidates)

    if goal is None:
        # No houses left: close on the enemy if we can win, else hold.
        for cell, contents in state.cells.items():
            if contents[enemy_idx] > 0 and beats_deterministically(
                count, contents[enemy_idx]
            ):
                goal = cell
                break
        if goal is None:
            return None

    best = _step_towards(state, coord, goal, count, idx, enemy_idx, odds_floor)
    if best is None:
        return None
    return best, 6.0 + goal_value


def _step_towards(
    state: GameState,
    coord: Coord,
    goal: Coord,
    count: int,
    idx: int,
    enemy_idx: int,
    odds_floor: float,
    forbid: Optional[Coord] = None,
) -> Optional[Coord]:
    """The neighbour that gets closest to `goal` without starting a bad fight."""
    best, best_distance = None, chebyshev(coord, goal)
    for cell in state.neighbours(coord):
        if cell == forbid:
            continue
        if _is_losing_step(state, cell, count, idx, enemy_idx, odds_floor):
            continue
        distance = chebyshev(cell, goal)
        if distance < best_distance:
            best, best_distance = cell, distance
    return best


def _split_towards_two_houses(
    state: GameState,
    coord: Coord,
    count: int,
    species: str,
    idx: int,
    enemy_idx: int,
    odds_floor: float = MIN_BATTLE_ODDS,
) -> Optional[Tuple[List[Move], float]]:
    """Divide a group so it can harvest two houses at once.

    The existing split intent only fires when two houses are *adjacent* to the
    group, which is rare, so in practice the agent almost never split: measured
    at 1.0% of its turns, against 67% for the random agent. Everything the
    policy saw about splitting therefore came from random partial moves, and a
    strategy it never sees proposed is one it can never learn to use.

    Splitting matters here because harvesting is parallel and the clock is
    short. Each side gets about 100 turns, a 50x50 board is 49 steps across,
    and one stack walking from house to house simply runs out of moves. Two
    half-stacks that can each afford their own house collect twice as fast.

    Both halves must be able to take their house outright -- a fragment that
    cannot convert anything is worse than no split at all, which is the lesson
    that cost 42 creatures of harvest when it was learnt the other way round.
    """
    affordable = [
        (humans * (0.95 ** chebyshev(coord, cell)), cell, humans)
        for cell, humans in state.humans()
        if humans <= count and chebyshev(coord, cell) > 0
    ]
    if len(affordable) < 2:
        return None
    affordable.sort(reverse=True)

    # Only the few best houses are worth pairing; this stays O(1) per group.
    for i in range(min(3, len(affordable))):
        for j in range(i + 1, min(4, len(affordable))):
            _, first, first_humans = affordable[i]
            _, second, second_humans = affordable[j]
            if first_humans + second_humans > count:
                continue

            step_one = _step_towards(
                state, coord, first, count, idx, enemy_idx, odds_floor
            )
            if step_one is None:
                continue
            step_two = _step_towards(
                state, coord, second, count, idx, enemy_idx, odds_floor,
                forbid=step_one,
            )
            if step_two is None:
                continue

            # The spare creatures go with the richer house, so the larger
            # half keeps the most options open.
            spare = count - first_humans - second_humans
            larger = first_humans + spare
            smaller = second_humans

            # Splitting trades safety for speed: two half-stacks harvest
            # twice as fast, but each is individually easier to kill. Measured
            # against the naive agent, which keeps one big stack, unguarded
            # splitting turned 2 losses in 80 games into 5 -- it walks a half
            # into range of something that outnumbers it by the 1.5x bar. So
            # neither half may end up where an enemy could already beat it.
            if _would_be_overpowered(
                state, step_one, larger, enemy_idx
            ) or _would_be_overpowered(state, step_two, smaller, enemy_idx):
                continue

            moves = [
                Move(*coord, larger, *step_one),
                Move(*coord, smaller, *step_two),
            ]
            return moves, 16.0 * (first_humans + second_humans)
    return None


def _would_be_overpowered(
    state: GameState, cell: Coord, count: int, enemy_idx: int
) -> bool:
    """Could any enemy group reach `cell` and wipe out `count` creatures?

    Two steps of slack: an enemy that far away can be adjacent after one move
    and attack on the next, which is exactly the window a freshly split half
    has to survive.
    """
    for enemy_cell, contents in state.cells.items():
        enemy = contents[enemy_idx]
        if enemy <= 0:
            continue
        if chebyshev(cell, enemy_cell) <= 2 and beats_deterministically(enemy, count):
            return True
    return False


def _retreat_step(
    state: GameState,
    coord: Coord,
    count: int,
    species: str,
    idx: int,
    enemy_idx: int,
    odds_floor: float = MIN_BATTLE_ODDS,
) -> Optional[Coord]:
    """Step away from an adjacent enemy strong enough to wipe us out."""
    # Two steps, not one. An enemy that far away is adjacent after its next
    # move and attacks on the one after, so reacting only once it is already
    # next door is reacting a turn too late. Traced on one map: a 51-strong
    # group sat within reach of a 102-strong stack for ten plies and was then
    # killed, costing a wipe-out win.
    threats = [
        cell
        for cell, contents in state.cells.items()
        if contents[enemy_idx] > 0
        and chebyshev(coord, cell) <= 2
        and beats_deterministically(contents[enemy_idx], count)
    ]
    if not threats:
        return None

    best, best_clearance = None, -1
    for cell in state.neighbours(coord):
        if _is_losing_step(state, cell, count, idx, enemy_idx, odds_floor):
            continue
        clearance = min(chebyshev(cell, threat) for threat in threats)
        if clearance > best_clearance:
            best, best_clearance = cell, clearance
    return best


def _should_mass(state: GameState, species: str, enemy_idx: int) -> bool:
    """True when the whole force could win a fight that no one group can.

    The 1.5x bar is a threshold on a single cell, so 156 creatures in three
    groups of 52 cannot kill 102 enemies even though 153 of them in one place
    could. Once the houses are gone there is nothing else to do with the
    spare turns, so the force should combine; while houses remain, splitting
    to harvest is worth more.
    """
    if state.humans():
        return False
    mine = state.groups(species)
    enemies = [
        contents[enemy_idx]
        for contents in state.cells.values()
        if contents[enemy_idx] > 0
    ]
    if not mine or not enemies:
        return False
    strongest = max(enemies)
    if any(beats_deterministically(count, strongest) for _, count in mine):
        return False  # we can already execute the kill, so do not shuffle
    return beats_deterministically(sum(count for _, count in mine), strongest)


def _regroup_step(
    state: GameState,
    coord: Coord,
    count: int,
    species: str,
    idx: int,
    enemy_idx: int,
    odds_floor: float = MIN_BATTLE_ODDS,
) -> Optional[Tuple[Coord, float]]:
    """Walk a detached group back towards our main stack.

    This matters most once the houses are gone: a small group on its own can
    only be caught and killed, while the same creatures inside the main stack
    help it clear the 1.5x bar. It is offered at any time but priced higher
    when there is nothing left to harvest.
    """
    others = [(at, n) for at, n in state.groups(species) if at != coord]
    if not others:
        return None

    massing = _should_mass(state, species, enemy_idx)
    if massing:
        # Converge on the nearest ally from both sides. Making only the
        # smaller group walk leaves it crossing open board alone, which is
        # exactly when the enemy stack catches it, and halves the rate at
        # which the two actually close.
        rally, _ = min(others, key=lambda g: chebyshev(coord, g[0]))
    else:
        rally, rally_size = max(others, key=lambda g: g[1])
        if rally_size < count:
            return None  # we are the main stack; let the others come to us

    best, best_distance = None, chebyshev(coord, rally)
    for cell in state.neighbours(coord):
        if _is_losing_step(state, cell, count, idx, enemy_idx, odds_floor):
            continue
        distance = chebyshev(cell, rally)
        if distance < best_distance:
            best, best_distance = cell, distance
    if best is None:
        return None
    # Massing is the whole plan in that position, so it outbids the ordinary
    # intents; otherwise merging is a mild tidy-up next to harvesting.
    value = 30.0 if massing else (14.0 if not state.humans() else 3.0)
    return best, value


def _is_losing_step(
    state: GameState,
    cell: Coord,
    count: int,
    idx: int,
    enemy_idx: int,
    odds_floor: float = MIN_BATTLE_ODDS,
) -> bool:
    """True if stepping here starts a fight we should not take."""
    contents = state.get(cell)
    humans = contents[0]
    if humans > 0 and not converts_deterministically(count, humans):
        return True
    enemy = contents[enemy_idx]
    if enemy > 0 and not beats_deterministically(count, enemy):
        return win_probability(count, enemy) < odds_floor
    return False
