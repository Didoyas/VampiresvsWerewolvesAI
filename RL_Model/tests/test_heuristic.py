"""The heuristic agent and the evaluation behind it.

Several of these lock in behaviour that was wrong at first and cost real
strength; the comments say which, so a later refactor does not quietly undo
the fix.
"""

import random

import pytest

from vvw import mapgen
from vvw.agents import heuristic, naive
from vvw.evaluate import DEFAULT_WEIGHTS, evaluate
from vvw.rules import Move, validate_turn
from vvw.simulator import apply_turn
from vvw.state import GameState, VAMPIRES, WEREWOLVES


def board(rows=10, columns=10) -> GameState:
    return GameState(rows=rows, columns=columns, me=VAMPIRES)


# ------------------------------------------------------------------ legality


def test_the_agent_always_proposes_a_legal_turn():
    rng = random.Random(4)
    for _ in range(200):
        state = _random_state(rng)
        if not state.groups(state.me):
            continue
        turn = heuristic.choose_turn(state)
        assert turn, "rule 1: a turn needs at least one move"
        assert validate_turn(state, turn) == []


@pytest.mark.parametrize("seed", [0, 1, 2, 3, 4])
def test_it_plays_generated_maps_legally_as_both_species(seed):
    state = mapgen.generate(seed)
    rng = random.Random(seed)
    for ply in range(40):
        species = VAMPIRES if ply % 2 == 0 else WEREWOLVES
        state.me = species
        turn = heuristic.choose_turn(state, species)
        assert validate_turn(state, turn, species) == []
        state = apply_turn(state, turn, species, rng=rng, validate=False)


# ------------------------------------------------------------------ tactics


def test_it_converts_a_house_it_can_take():
    state = board()
    state.set_cell((4, 4), (0, 8, 0))
    state.set_cell((4, 5), (5, 0, 0))
    turn = heuristic.choose_turn(state)
    assert any(move.target == (4, 5) for move in turn)


def test_it_takes_the_whole_group_to_a_house_by_default():
    """Sending only what the house needs strands the remainder.

    Splitting 11 into 8 and 7 to take a 4-house leaves neither half able to
    ever take a 19-house, while 11 kept whole grows 11 -> 15 -> 19 and takes
    two of them. This cost 42 creatures of harvest on one map alone.
    """
    state = board(20, 20)
    state.set_cell((5, 5), (0, 11, 0))
    state.set_cell((5, 6), (4, 0, 0))
    state.set_cell((15, 15), (19, 0, 0))
    state.set_cell((16, 16), (19, 0, 0))
    turn = heuristic.choose_turn(state)
    conversion = [m for m in turn if m.target == (5, 6)]
    assert conversion, "it should take the house"
    assert conversion[0].count == 11, "it should go in with everything"


def test_it_refuses_a_fight_it_would_lose():
    state = board()
    state.set_cell((4, 4), (0, 4, 0))
    state.set_cell((4, 5), (0, 0, 20))
    turn = heuristic.choose_turn(state)
    assert all(move.target != (4, 5) for move in turn)


def test_it_attacks_when_it_has_the_one_and_a_half_bar():
    state = board()
    state.set_cell((4, 4), (0, 9, 0))
    state.set_cell((4, 5), (0, 0, 6))
    turn = heuristic.choose_turn(state)
    assert any(move.target == (4, 5) for move in turn)


def test_it_does_not_stall_next_to_a_house_it_cannot_take():
    """Standing beside an unconvertible house used to freeze the agent.

    It would pick the biggest house as its goal, walk up to it, find no
    neighbouring cell closer, and bounce on the spot for the rest of the game,
    stuck on its starting population.
    """
    state = board(20, 20)
    state.set_cell((5, 5), (0, 10, 0))
    state.set_cell((5, 6), (30, 0, 0))  # adjacent, far too big
    state.set_cell((14, 14), (8, 0, 0))  # reachable, and takeable

    positions = []
    current = state
    for _ in range(14):
        current.me = VAMPIRES
        turn = heuristic.choose_turn(current, VAMPIRES)
        current = apply_turn(current, turn, VAMPIRES, rng=random.Random(0), validate=False)
        positions.append(next(iter(sorted(current.groups(VAMPIRES))))[0])

    assert len(set(positions)) > 3, f"the agent is oscillating: {positions}"


def test_it_grows_on_a_map_it_has_to_walk_across():
    state = mapgen.generate(100004)
    start = state.population(VAMPIRES)
    rng = random.Random(7)
    for _ in range(100):
        state.me = VAMPIRES
        turn = heuristic.choose_turn(state, VAMPIRES)
        state = apply_turn(state, turn, VAMPIRES, rng=rng, validate=False)
    assert state.population(VAMPIRES) > start * 2


# --------------------------------------------------------------- evaluation


def test_more_creatures_scores_better():
    weak, strong = board(), board()
    weak.set_cell((0, 0), (0, 5, 0))
    weak.set_cell((9, 9), (0, 0, 5))
    strong.set_cell((0, 0), (0, 20, 0))
    strong.set_cell((9, 9), (0, 0, 5))
    assert evaluate(strong, VAMPIRES) > evaluate(weak, VAMPIRES)


def test_the_score_is_symmetric_between_the_species():
    state = board()
    state.set_cell((0, 0), (0, 9, 0))
    state.set_cell((9, 9), (0, 0, 4))
    assert evaluate(state, VAMPIRES) > evaluate(state, WEREWOLVES)


def test_being_closer_to_a_house_scores_better():
    near, far = board(20, 20), board(20, 20)
    for state, where in ((near, (5, 5)), (far, (18, 18))):
        state.set_cell(where, (0, 10, 0))
        state.set_cell((0, 19), (0, 0, 10))
        state.set_cell((4, 4), (6, 0, 0))
    assert evaluate(near, VAMPIRES) > evaluate(far, VAMPIRES)


def test_an_extra_group_that_cannot_take_anything_is_not_rewarded():
    """Claiming houses a group cannot convert paid for pointless splitting."""
    whole, split = board(20, 20), board(20, 20)
    for state in (whole, split):
        state.set_cell((15, 15), (19, 0, 0))
        state.set_cell((16, 16), (19, 0, 0))
        state.set_cell((0, 19), (0, 0, 11))
    whole.set_cell((5, 5), (0, 11, 0))
    split.set_cell((5, 5), (0, 6, 0))
    split.set_cell((5, 6), (0, 5, 0))
    assert evaluate(whole, VAMPIRES) >= evaluate(split, VAMPIRES)


def test_being_wiped_out_scores_worse_than_any_surviving_position():
    wiped = board()
    wiped.set_cell((9, 9), (0, 0, 5))

    hanging_on = board()
    hanging_on.set_cell((0, 0), (0, 1, 0))  # one creature left, far behind
    hanging_on.set_cell((9, 9), (0, 0, 200))

    assert evaluate(wiped, VAMPIRES) < evaluate(hanging_on, VAMPIRES)


def test_wiping_the_enemy_out_scores_better_than_any_ongoing_position():
    won = board()
    won.set_cell((0, 0), (0, 1, 0))

    ahead = board()
    ahead.set_cell((0, 0), (0, 200, 0))
    ahead.set_cell((9, 9), (0, 0, 1))

    assert evaluate(won, VAMPIRES) > evaluate(ahead, VAMPIRES)


# ------------------------------------------------------------------ strength


def test_it_harvests_at_least_as_well_as_the_naive_agent():
    """Growth with no opponent, which is the skill that decides most games."""
    totals = {}
    for name, agent in (("naive", naive.choose_turn), ("heuristic", heuristic.choose_turn)):
        total = 0
        for seed in mapgen.seeds("validation", 6):
            state = mapgen.generate(seed)
            rng = random.Random(7)
            for _ in range(100):
                state.me = VAMPIRES
                turn = agent(state, VAMPIRES)
                state = apply_turn(state, turn, VAMPIRES, rng=rng, validate=False)
            total += state.population(VAMPIRES)
        totals[name] = total
    assert totals["heuristic"] >= totals["naive"] * 0.95, totals


def test_a_decision_is_far_inside_the_two_second_budget():
    import time

    state = mapgen.generate(9, rows=50, columns=50)
    state.me = VAMPIRES
    for _ in range(12):  # let some groups accumulate first
        state = apply_turn(
            state,
            heuristic.choose_turn(state, VAMPIRES),
            VAMPIRES,
            rng=random.Random(1),
            validate=False,
        )
    started = time.perf_counter()
    heuristic.choose_turn(state, VAMPIRES)
    assert time.perf_counter() - started < 0.25


def _random_state(rng: random.Random) -> GameState:
    rows, columns = rng.randint(4, 12), rng.randint(4, 12)
    state = GameState(rows=rows, columns=columns, me=VAMPIRES)
    for _ in range(rng.randint(1, 8)):
        cell = (rng.randrange(columns), rng.randrange(rows))
        if cell in state.cells:
            continue
        kind = rng.choice([0, 1, 2])
        count = rng.randint(1, 30)
        state.set_cell(cell, tuple(count if i == kind else 0 for i in range(3)))
    return state


# ---------------------------------------------------------- endgame massing


def test_it_masses_when_only_the_whole_force_can_win():
    """The 1.5x bar is a threshold on one cell, not on the army.

    Traced on a real game: 156 creatures in three groups of ~52 could not kill
    102 enemies, even though 153 of them in one place could. The agent let a
    51-strong group sit inside the enemy's reach for ten plies, lost it, and
    turned a wipe-out into a three-creature win.
    """
    from vvw.agents.heuristic import _should_mass
    from vvw.state import WEREWOLVES, species_index

    enemy_index = species_index(WEREWOLVES)

    scattered = GameState(rows=20, columns=20, me=VAMPIRES)
    scattered.set_cell((2, 2), (0, 54, 0))
    scattered.set_cell((9, 9), (0, 51, 0))
    scattered.set_cell((16, 16), (0, 51, 0))
    scattered.set_cell((18, 2), (0, 0, 102))
    assert _should_mass(scattered, VAMPIRES, enemy_index)

    # Already able to execute: do not waste turns shuffling.
    ready = GameState(rows=20, columns=20, me=VAMPIRES)
    ready.set_cell((2, 2), (0, 156, 0))
    ready.set_cell((18, 2), (0, 0, 102))
    assert not _should_mass(ready, VAMPIRES, enemy_index)

    # Not enough even combined: massing would not help.
    hopeless = GameState(rows=20, columns=20, me=VAMPIRES)
    hopeless.set_cell((2, 2), (0, 30, 0))
    hopeless.set_cell((9, 9), (0, 30, 0))
    hopeless.set_cell((18, 2), (0, 0, 102))
    assert not _should_mass(hopeless, VAMPIRES, enemy_index)


def test_it_does_not_mass_while_there_are_still_houses_to_take():
    """Harvesting in parallel is worth more than being tidy."""
    from vvw.agents.heuristic import _should_mass
    from vvw.state import WEREWOLVES, species_index

    state = GameState(rows=20, columns=20, me=VAMPIRES)
    state.set_cell((2, 2), (0, 54, 0))
    state.set_cell((9, 9), (0, 51, 0))
    state.set_cell((18, 2), (0, 0, 102))
    state.set_cell((5, 15), (8, 0, 0))
    assert not _should_mass(state, VAMPIRES, species_index(WEREWOLVES))


def test_a_threatened_group_reacts_before_the_enemy_is_adjacent():
    """One step of warning is a step too late: it attacks on the next turn."""
    state = GameState(rows=12, columns=12, me=VAMPIRES)
    state.set_cell((5, 5), (0, 20, 0))
    state.set_cell((7, 7), (0, 0, 60))  # two steps away, and overwhelming

    from vvw.evaluate import chebyshev

    turn = heuristic.choose_turn(state, VAMPIRES)
    moved = [m for m in turn if m.source == (5, 5)]
    assert moved, "it should not stand still next to something that beats it"
    assert all(
        chebyshev(m.target, (7, 7)) >= chebyshev((5, 5), (7, 7)) for m in moved
    ), "it should not step towards the thing that outnumbers it"
