"""Game rules: move legality and combat.

Everything the PDF leaves open is collected in `Unknowns` rather than being
decided quietly somewhere in the code, so that when the real server tells us
the answer there is exactly one place to change.

Combat, from the PDF:

  against humans    attackers >= humans   -> convert them all, no losses
                    attackers <  humans   -> random battle
  against creatures attackers >= 1.5x     -> kill them all, no losses
                    defenders >= 1.5x     -> every attacker dies
                    otherwise             -> random battle

  random battle, E1 attackers against E2 defenders, P = P(attackers win):
      E1 == E2 -> 0.5
      E1 <  E2 -> E1 / (2 * E2)
      E1 >  E2 -> E1 / E2 - 0.5
  if the attackers win,  each attacker survives with probability P, and
      against humans each human survives *and converts* with probability P;
  if the attackers lose, each defender survives with probability 1 - P.

The 1.5 ratio is compared as `2 * E1 >= 3 * E2` to keep it in integers.
"""

from __future__ import annotations

import random
from dataclasses import dataclass
from typing import List, NamedTuple, Sequence, Tuple

from .state import GameState, Coord, opponent_of, species_index


class Move(NamedTuple):
    sx: int
    sy: int
    count: int
    tx: int
    ty: int

    @property
    def source(self) -> Coord:
        return (self.sx, self.sy)

    @property
    def target(self) -> Coord:
        return (self.tx, self.ty)

    def as_wire(self) -> Tuple[int, int, int, int, int]:
        return (self.sx, self.sy, self.count, self.tx, self.ty)


Turn = Sequence[Move]


@dataclass(frozen=True)
class Unknowns:
    """Rules the PDF does not pin down.  Confirm each against the server.

    Defaults follow the PDF where it speaks, and the user's notes otherwise;
    every one of them is still marked unconfirmed in docs/open_questions.md.
    """

    # The PDF says a human who survives a won random battle is converted.
    # Shrey's report claims the opposite as a team clarification.
    humans_convert_in_random_battle: bool = True
    # Confirmed by the freewin probe: 6 vampires took 4 werewolves and the
    # cell ended holding all 6.  A deterministic win costs nothing.
    deterministic_win_is_free: bool = True
    # Confirmed: MaxPlays=200 counts turns for the whole game, not per player,
    # so two alternating players get about 100 turns each.  The server config
    # comments it "Nombre de coups max pour une partie", the one-player probe
    # stopped at Coup n 200, and the user confirmed 100 each.
    max_plays_counts_per_player: bool = False
    max_plays: int = 200
    max_game_seconds: int = 300
    # Confirmed by the user: at either cap the creatures on the board are
    # compared, and equal counts are a genuine draw. There is no tie-break
    # against whoever reached the cap first. Humans never count.
    tie_at_cap_loses_for_first_mover: bool = False
    # Confirmed by the merge probe: two groups of 3 entering one cell beat
    # 4 defenders outright, so they merged to 6 before the fight rather than
    # resolving as two separate 3-vs-4 random battles.
    moves_into_one_target_merge_first: bool = True
    # Counts travel as one byte on the wire.
    max_count_per_move: int = 255


DEFAULT_UNKNOWNS = Unknowns()


# ---------------------------------------------------------------------------
# Combat
# ---------------------------------------------------------------------------


def win_probability(attackers: int, defenders: int) -> float:
    """P that `attackers` win a random battle against `defenders`."""
    if attackers <= 0:
        return 0.0
    if defenders <= 0:
        return 1.0
    if attackers == defenders:
        return 0.5
    if attackers < defenders:
        return attackers / (2.0 * defenders)
    return min(1.0, attackers / defenders - 0.5)


def beats_deterministically(attackers: int, defenders: int) -> bool:
    """True when attackers reach the 1.5x ratio over enemy creatures."""
    return 2 * attackers >= 3 * defenders


def converts_deterministically(attackers: int, humans: int) -> bool:
    """True when attackers are at least as numerous as the humans."""
    return attackers >= humans


class BattleOutcome(NamedTuple):
    """What is left on the contested cell afterwards."""

    attackers: int
    defenders: int  # surviving enemy creatures, or humans that stayed human
    converted: int  # humans that joined the attacker


def resolve_vs_humans(
    attackers: int,
    humans: int,
    rng: random.Random | None = None,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
) -> BattleOutcome:
    """Resolve an attack on a human house.

    With `rng` the per-creature coin flips are sampled; without it the
    deterministic branches still work and a random battle raises, because
    there is no single answer to return.
    """
    if humans == 0:
        return BattleOutcome(attackers=attackers, defenders=0, converted=0)
    if converts_deterministically(attackers, humans):
        return BattleOutcome(attackers=attackers, defenders=0, converted=humans)
    if rng is None:
        raise ValueError("a random battle needs an rng")

    p = win_probability(attackers, humans)
    if rng.random() < p:
        survivors = _survivors(attackers, p, rng)
        converted = (
            _survivors(humans, p, rng)
            if unknowns.humans_convert_in_random_battle
            else 0
        )
        return BattleOutcome(attackers=survivors, defenders=0, converted=converted)
    remaining = _survivors(humans, 1.0 - p, rng)
    return BattleOutcome(attackers=0, defenders=remaining, converted=0)


def resolve_vs_creatures(
    attackers: int,
    defenders: int,
    rng: random.Random | None = None,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
) -> BattleOutcome:
    """Resolve an attack on enemy creatures."""
    if defenders == 0:
        return BattleOutcome(attackers=attackers, defenders=0, converted=0)
    if beats_deterministically(attackers, defenders):
        return BattleOutcome(attackers=attackers, defenders=0, converted=0)
    if beats_deterministically(defenders, attackers):
        return BattleOutcome(attackers=0, defenders=defenders, converted=0)
    if rng is None:
        raise ValueError("a random battle needs an rng")

    p = win_probability(attackers, defenders)
    if rng.random() < p:
        return BattleOutcome(attackers=_survivors(attackers, p, rng), defenders=0, converted=0)
    return BattleOutcome(
        attackers=0, defenders=_survivors(defenders, 1.0 - p, rng), converted=0
    )


def _survivors(count: int, probability: float, rng: random.Random) -> int:
    """Each of `count` creatures survives independently with `probability`."""
    return sum(1 for _ in range(count) if rng.random() < probability)


def likeliest_outcome_vs_creatures(attackers: int, defenders: int) -> BattleOutcome:
    """Collapse a battle to its likelier side, for planning only.

    Unlike `expected_outcome_*`, this returns whole creatures on one side
    only, so the result is a state a planner can keep searching from. It is
    not how the real game resolves; `resolve_*` is.
    """
    if defenders == 0 or beats_deterministically(attackers, defenders):
        return BattleOutcome(attackers=attackers, defenders=0, converted=0)
    if beats_deterministically(defenders, attackers):
        return BattleOutcome(attackers=0, defenders=defenders, converted=0)

    p = win_probability(attackers, defenders)
    if p >= 0.5:
        return BattleOutcome(max(1, round(p * attackers)), 0, 0)
    return BattleOutcome(0, max(1, round((1.0 - p) * defenders)), 0)


def likeliest_outcome_vs_humans(
    attackers: int, humans: int, unknowns: Unknowns = DEFAULT_UNKNOWNS
) -> BattleOutcome:
    """As above, against a house.

    Understrength attacks always collapse to a loss, because P = E1/(2*E2)
    is below 0.5 whenever E1 < E2. That is the right discouragement.
    """
    if humans == 0:
        return BattleOutcome(attackers=attackers, defenders=0, converted=0)
    if converts_deterministically(attackers, humans):
        return BattleOutcome(attackers=attackers, defenders=0, converted=humans)

    p = win_probability(attackers, humans)
    if p >= 0.5:
        converted = (
            round(p * humans) if unknowns.humans_convert_in_random_battle else 0
        )
        return BattleOutcome(max(1, round(p * attackers)), 0, converted)
    return BattleOutcome(0, max(1, round((1.0 - p) * humans)), 0)


def expected_outcome_vs_humans(attackers: int, humans: int) -> Tuple[float, float]:
    """(expected attacker-side creatures, expected humans left) in closed form.

    On a win both the surviving attackers and the converted humans end up on
    our side, so the attacker side is P * (E1 + E2) and that case happens with
    probability P, giving P^2 * (E1 + E2).
    """
    if humans == 0:
        return float(attackers), 0.0
    if converts_deterministically(attackers, humans):
        return float(attackers + humans), 0.0
    p = win_probability(attackers, humans)
    return p * p * (attackers + humans), (1.0 - p) * (1.0 - p) * humans


def expected_outcome_vs_creatures(attackers: int, defenders: int) -> Tuple[float, float]:
    """(expected surviving attackers, expected surviving defenders)."""
    if defenders == 0:
        return float(attackers), 0.0
    if beats_deterministically(attackers, defenders):
        return float(attackers), 0.0
    if beats_deterministically(defenders, attackers):
        return 0.0, float(defenders)
    p = win_probability(attackers, defenders)
    return p * p * attackers, (1.0 - p) * (1.0 - p) * defenders


# ---------------------------------------------------------------------------
# Move legality
# ---------------------------------------------------------------------------


def is_adjacent(a: Coord, b: Coord) -> bool:
    """Eight-way adjacency; a cell is not adjacent to itself."""
    dx, dy = abs(a[0] - b[0]), abs(a[1] - b[1])
    return max(dx, dy) == 1


def validate_turn(
    state: GameState,
    turn: Turn,
    species: str | None = None,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
) -> List[str]:
    """Check a whole turn against rules 1-6 and return the violations.

    An empty list means the turn is legal.  Rules 3 and 5 are properties of
    the turn as a whole, not of any single move, which is why this takes the
    whole plan.
    """
    species = species or state.me
    idx = species_index(species)
    problems: List[str] = []

    if len(turn) == 0:
        problems.append("rule 1: a turn must contain at least one move")
    if len(turn) > 255:
        problems.append(f"wire: {len(turn)} moves do not fit in one byte")

    leaving: dict = {}
    sources, targets = set(), set()

    for i, move in enumerate(turn):
        where = f"move {i} {tuple(move)}"

        if not state.in_bounds(move.sx, move.sy):
            problems.append(f"{where}: source is off the board")
            continue
        if not state.in_bounds(move.tx, move.ty):
            problems.append(f"{where}: rule 4: target is off the board")
            continue
        if not is_adjacent(move.source, move.target):
            problems.append(f"{where}: rule 4: target is not one of the 8 neighbours")
            continue

        if move.count < 1:
            problems.append(f"{where}: rule 6: must move at least one creature")
            continue
        if move.count > unknowns.max_count_per_move:
            problems.append(f"{where}: wire: count {move.count} does not fit in a byte")

        available = state.get(move.source)[idx]
        if available <= 0:
            problems.append(f"{where}: rule 2: source holds none of our {species}")
            continue

        leaving[move.source] = leaving.get(move.source, 0) + move.count
        sources.add(move.source)
        targets.add(move.target)

    for coord, total in leaving.items():
        available = state.get(coord)[idx]
        if total > available:
            problems.append(
                f"rule 3: cell {coord} sends {total} but holds only {available}"
            )

    for coord in sources & targets:
        problems.append(f"rule 5: cell {coord} is both a source and a target")

    return problems


def is_legal_turn(state: GameState, turn: Turn, species: str | None = None) -> bool:
    return not validate_turn(state, turn, species)
