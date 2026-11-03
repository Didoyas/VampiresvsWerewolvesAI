"""Playing agents against each other on our own simulator.

Comparisons here are *paired*: each map is played four times, so that every
pair of agents meets on the same board as both species and in both move
orders. Without that, a result mostly measures who drew the better side.

    python tools/match.py --a heuristic --b naive --maps 50
    python tools/match.py --a heuristic --b naive --split test --maps 100

Win rates come with a Wilson interval, which behaves sensibly near 0 and 1
where the textbook normal interval does not.
"""

from __future__ import annotations

import argparse
import math
import random
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from vvw import mapgen  # noqa: E402
from vvw.agents import aggressive, heuristic, naive, random_legal  # noqa: E402
from vvw.rules import DEFAULT_UNKNOWNS, Unknowns, validate_turn  # noqa: E402
from vvw.simulator import apply_turn, is_terminal, winner  # noqa: E402
from vvw.state import (  # noqa: E402
    GameState,
    VAMPIRES,
    WEREWOLVES,
    opponent_of,
    species_index,
)

Agent = Callable[[GameState, str], List]

AGENTS: Dict[str, Agent] = {
    "random": random_legal.choose_turn,
    "naive": naive.choose_turn,
    "aggressive": aggressive.choose_turn,
    "heuristic": heuristic.choose_turn,
}


@dataclass
class Result:
    wins_a: int = 0
    wins_b: int = 0
    draws: int = 0
    illegal_a: int = 0
    illegal_b: int = 0
    margins: List[int] = field(default_factory=list)
    decision_times: List[float] = field(default_factory=list)

    @property
    def games(self) -> int:
        return self.wins_a + self.wins_b + self.draws


def play(
    agent_a: Agent,
    agent_b: Agent,
    state: GameState,
    rng: random.Random,
    a_plays: str,
    a_moves_first: bool,
    result: Result,
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
) -> Tuple[str | None, int]:
    """Play one game out. Returns (winning species, A's population margin).

    An agent that proposes an illegal turn forfeits its move; that is recorded
    rather than hidden, because it should never happen.
    """
    b_plays = opponent_of(a_plays)
    order = [a_plays, b_plays] if a_moves_first else [b_plays, a_plays]
    first_mover = order[0]
    who: Dict[str, Agent] = {a_plays: agent_a, b_plays: agent_b}

    turn_index = 0
    while not is_terminal(state, unknowns):
        species = order[turn_index % 2]
        state.me = species
        started = time.perf_counter()
        moves = who[species](state, species)
        result.decision_times.append(time.perf_counter() - started)

        if moves and validate_turn(state, moves, species, unknowns):
            if species == a_plays:
                result.illegal_a += 1
            else:
                result.illegal_b += 1
            moves = []

        if moves:
            state = apply_turn(
                state, moves, species, rng=rng, unknowns=unknowns, validate=False
            )
        else:
            state.plies[species] = state.plies.get(species, 0) + 1
        turn_index += 1

    # Equal creature counts are a genuine draw, so draws are reported as
    # draws and an agent has to actually win on creatures.
    champion = winner(state, None, unknowns)
    margin = state.population(a_plays) - state.population(b_plays)
    return champion, margin


def handicap(state: GameState, species: str, factor: float) -> GameState:
    """Scale one species' starting force.

    Generated maps are point-symmetric, so two agents of similar strength
    harvest mirror halves and finish exactly level: on 50 maps that was 154
    draws out of 200 games, which measures almost nothing. Starting one side
    short breaks the mirror and asks the questions that actually matter — can
    this agent convert a lead, and can it survive a deficit?
    """
    if factor == 1.0:
        return state
    for coord, count in state.groups(species):
        contents = list(state.get(coord))
        contents[species_index(species)] = max(1, int(round(count * factor)))
        state.set_cell(coord, tuple(contents))
    return state


def paired_series(
    agent_a: Agent,
    agent_b: Agent,
    seeds: List[int],
    unknowns: Unknowns = DEFAULT_UNKNOWNS,
    a_handicap: float = 1.0,
) -> Result:
    """Each seed played four ways: both species crossed with both move orders."""
    result = Result()
    for seed in seeds:
        for a_plays in (VAMPIRES, WEREWOLVES):
            for a_first in (True, False):
                state = handicap(mapgen.generate(seed), a_plays, a_handicap)
                random_legal.seed(seed)
                rng = random.Random(seed * 7919 + hash((a_plays, a_first)) % 1000)
                champion, margin = play(
                    agent_a, agent_b, state, rng, a_plays, a_first, result, unknowns
                )
                result.margins.append(margin)
                if champion is None:
                    result.draws += 1
                elif champion == a_plays:
                    result.wins_a += 1
                else:
                    result.wins_b += 1
    return result


def wilson_interval(successes: int, total: int, z: float = 1.96) -> Tuple[float, float]:
    """95% confidence interval for a proportion, usable near 0 and 1."""
    if total == 0:
        return (0.0, 1.0)
    phat = successes / total
    denominator = 1 + z * z / total
    centre = (phat + z * z / (2 * total)) / denominator
    spread = (
        z
        * math.sqrt(phat * (1 - phat) / total + z * z / (4 * total * total))
        / denominator
    )
    return (max(0.0, centre - spread), min(1.0, centre + spread))


def report(name_a: str, name_b: str, result: Result) -> str:
    total = result.games
    # A draw is not a win; it counts against both win rates.
    low, high = wilson_interval(result.wins_a, total)
    decisive = result.wins_a + result.wins_b
    times = sorted(result.decision_times)

    def percentile(fraction: float) -> float:
        if not times:
            return 0.0
        return times[min(len(times) - 1, int(fraction * len(times)))]

    lines = [
        f"{total} games, paired across both species and both move orders",
        f"  {name_a:<12} {result.wins_a:>4} wins  "
        f"({result.wins_a / total:.1%}, 95% CI {low:.1%}-{high:.1%})",
        f"  {name_b:<12} {result.wins_b:>4} wins  ({result.wins_b / total:.1%})",
        f"  draws        {result.draws:>4}"
        + (
            f"   ({result.wins_a}/{decisive} = {result.wins_a / decisive:.1%} "
            "of decided games)"
            if decisive
            else "   (every game drawn)"
        ),
        f"  mean margin for {name_a}: "
        f"{sum(result.margins) / len(result.margins):+.1f} creatures",
        f"  illegal turns: {name_a} {result.illegal_a}, {name_b} {result.illegal_b}",
        f"  decision time: p50 {percentile(0.5) * 1000:.1f} ms, "
        f"p95 {percentile(0.95) * 1000:.1f} ms, "
        f"max {max(times) * 1000:.1f} ms" if times else "",
    ]
    return "\n".join(line for line in lines if line)


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--a", default="heuristic", choices=sorted(AGENTS))
    parser.add_argument("--b", default="naive", choices=sorted(AGENTS))
    parser.add_argument("--maps", type=int, default=25)
    parser.add_argument("--split", default="validation", choices=sorted(mapgen.SPLITS))
    parser.add_argument(
        "--handicap",
        type=float,
        default=1.0,
        help="scale A's starting force; below 1.0 breaks the mirror and "
        "forces decisive games",
    )
    args = parser.parse_args(argv)

    seeds = mapgen.seeds(args.split, args.maps)
    started = time.perf_counter()
    result = paired_series(
        AGENTS[args.a], AGENTS[args.b], seeds, a_handicap=args.handicap
    )
    elapsed = time.perf_counter() - started

    print(
        f"{args.a} vs {args.b} on {args.maps} {args.split} maps"
        + (f" (A handicap {args.handicap})" if args.handicap != 1.0 else "")
    )
    print(report(args.a, args.b, result))
    print(f"  wall clock: {elapsed:.1f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
