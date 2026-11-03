"""Rules: combat maths and move legality."""

import random

import pytest

from vvw.rules import (
    Move,
    beats_deterministically,
    converts_deterministically,
    expected_outcome_vs_creatures,
    expected_outcome_vs_humans,
    is_adjacent,
    resolve_vs_creatures,
    resolve_vs_humans,
    validate_turn,
    win_probability,
)
from vvw.state import GameState, VAMPIRES, WEREWOLVES


# ---------------------------------------------------------------- thresholds


@pytest.mark.parametrize(
    "attackers, humans, expected",
    [(4, 4, True), (5, 4, True), (3, 4, False), (1, 1, True), (0, 1, False)],
)
def test_conversion_threshold_is_greater_or_equal(attackers, humans, expected):
    assert converts_deterministically(attackers, humans) is expected


@pytest.mark.parametrize(
    "attackers, defenders, expected",
    [
        (3, 2, True),  # exactly 1.5x counts
        (6, 4, True),
        (5, 4, False),
        (2, 2, False),
        (9, 6, True),
        (8, 6, False),
    ],
)
def test_kill_threshold_is_one_and_a_half(attackers, defenders, expected):
    assert beats_deterministically(attackers, defenders) is expected


def test_one_and_a_half_uses_integer_arithmetic():
    # 1.5 * 34 == 51 exactly; a float comparison can round the wrong way.
    assert beats_deterministically(51, 34) is True
    assert beats_deterministically(50, 34) is False


# -------------------------------------------------------------- probabilities


def test_equal_forces_are_a_coin_flip():
    assert win_probability(10, 10) == 0.5


@pytest.mark.parametrize("attackers, defenders", [(1, 4), (3, 10), (5, 6)])
def test_outnumbered_attacker_formula(attackers, defenders):
    assert win_probability(attackers, defenders) == attackers / (2 * defenders)


@pytest.mark.parametrize("attackers, defenders", [(5, 4), (7, 5), (11, 10)])
def test_outnumbering_attacker_formula(attackers, defenders):
    assert win_probability(attackers, defenders) == pytest.approx(
        attackers / defenders - 0.5
    )


def test_probability_reaches_one_exactly_at_the_kill_threshold():
    # The spec's line passes through (1.5 * E2, 1), which is why 1.5x is the
    # deterministic threshold.
    assert win_probability(15, 10) == pytest.approx(1.0)


def test_probability_stays_within_zero_and_one():
    for attackers in range(0, 60):
        for defenders in range(1, 20):
            assert 0.0 <= win_probability(attackers, defenders) <= 1.0


def test_probability_is_monotonic_in_attacker_count():
    previous = -1.0
    for attackers in range(0, 40):
        current = win_probability(attackers, 12)
        assert current >= previous
        previous = current


# ------------------------------------------------------------------- battles


def test_meeting_humans_at_equal_strength_converts_without_a_battle():
    outcome = resolve_vs_humans(4, 4, rng=None)
    assert outcome == (4, 0, 4)  # nobody dies, every human joins


def test_outnumbering_enemy_by_half_wins_without_losses():
    outcome = resolve_vs_creatures(6, 4, rng=None)
    assert outcome == (6, 0, 0)


def test_being_outnumbered_by_half_kills_every_attacker():
    outcome = resolve_vs_creatures(4, 6, rng=None)
    assert outcome == (0, 6, 0)


def test_a_random_battle_needs_an_rng():
    with pytest.raises(ValueError):
        resolve_vs_creatures(5, 4, rng=None)
    with pytest.raises(ValueError):
        resolve_vs_humans(3, 4, rng=None)


def test_random_battle_outcomes_are_reproducible_from_a_seed():
    first = resolve_vs_creatures(5, 4, rng=random.Random(7))
    second = resolve_vs_creatures(5, 4, rng=random.Random(7))
    assert first == second


def test_a_random_battle_leaves_only_one_side_standing():
    rng = random.Random(0)
    for _ in range(200):
        outcome = resolve_vs_creatures(5, 4, rng=rng)
        assert outcome.attackers == 0 or outcome.defenders == 0


def test_sampled_survivors_match_the_closed_form_expectation():
    # The analytical formulas feed search leaves, so they have to agree with
    # what the simulator actually does.
    attackers, defenders = 5, 4
    trials = 20000
    rng = random.Random(12345)
    total_attackers = total_defenders = 0
    for _ in range(trials):
        outcome = resolve_vs_creatures(attackers, defenders, rng=rng)
        total_attackers += outcome.attackers
        total_defenders += outcome.defenders

    expected_attackers, expected_defenders = expected_outcome_vs_creatures(
        attackers, defenders
    )
    assert total_attackers / trials == pytest.approx(expected_attackers, abs=0.05)
    assert total_defenders / trials == pytest.approx(expected_defenders, abs=0.05)


def test_sampled_human_conversion_matches_the_closed_form_expectation():
    attackers, humans = 3, 5
    trials = 20000
    rng = random.Random(999)
    total_ours = 0
    for _ in range(trials):
        outcome = resolve_vs_humans(attackers, humans, rng=rng)
        total_ours += outcome.attackers + outcome.converted

    expected_ours, _ = expected_outcome_vs_humans(attackers, humans)
    assert total_ours / trials == pytest.approx(expected_ours, abs=0.05)


# ------------------------------------------------------------------ adjacency


def test_all_eight_neighbours_are_adjacent():
    assert sum(
        is_adjacent((5, 5), (5 + dx, 5 + dy))
        for dx in (-1, 0, 1)
        for dy in (-1, 0, 1)
    ) == 8


def test_a_cell_is_not_adjacent_to_itself_or_to_distant_cells():
    assert not is_adjacent((5, 5), (5, 5))
    assert not is_adjacent((5, 5), (7, 5))


# ------------------------------------------------------------------ legality


def board() -> GameState:
    """A 5x5 board: 10 of ours at (1,1), 4 of theirs at (3,3), 3 humans."""
    state = GameState(rows=5, columns=5, me=VAMPIRES)
    state.set_cell((1, 1), (0, 10, 0))
    state.set_cell((3, 3), (0, 0, 4))
    state.set_cell((0, 3), (3, 0, 0))
    return state


def test_a_plain_move_is_legal():
    assert validate_turn(board(), [Move(1, 1, 5, 1, 2)]) == []


def test_rule_1_an_empty_turn_is_rejected():
    problems = validate_turn(board(), [])
    assert any("rule 1" in p for p in problems)


def test_rule_2_cannot_move_the_other_species():
    problems = validate_turn(board(), [Move(3, 3, 1, 3, 2)])
    assert any("rule 2" in p for p in problems)


def test_rule_3_cannot_send_more_than_the_cell_holds():
    problems = validate_turn(board(), [Move(1, 1, 11, 1, 2)])
    assert any("rule 3" in p for p in problems)


def test_rule_3_counts_every_move_out_of_the_same_cell():
    # Each move is fine alone; together they overdraw the cell.
    turn = [Move(1, 1, 6, 1, 2), Move(1, 1, 6, 2, 1)]
    problems = validate_turn(board(), turn)
    assert any("rule 3" in p for p in problems)


def test_rule_4_rejects_a_target_off_the_board():
    state = board()
    state.set_cell((0, 0), (0, 4, 0))
    problems = validate_turn(state, [Move(0, 0, 1, -1, 0)])
    assert problems  # caught as off the board


def test_rule_4_rejects_a_jump_of_more_than_one_cell():
    problems = validate_turn(board(), [Move(1, 1, 5, 3, 1)])
    assert any("rule 4" in p for p in problems)


def test_rule_5_a_cell_cannot_be_source_and_target_at_once():
    state = board()
    state.set_cell((2, 2), (0, 4, 0))
    turn = [Move(1, 1, 5, 2, 2), Move(2, 2, 4, 3, 2)]
    problems = validate_turn(state, turn)
    assert any("rule 5" in p for p in problems)


def test_rule_6_rejects_moving_zero_creatures():
    problems = validate_turn(board(), [Move(1, 1, 0, 1, 2)])
    assert any("rule 6" in p for p in problems)


def test_splitting_one_group_across_two_cells_is_legal():
    turn = [Move(1, 1, 4, 1, 2), Move(1, 1, 6, 2, 1)]
    assert validate_turn(board(), turn) == []


def test_a_count_that_does_not_fit_in_a_byte_is_rejected():
    state = GameState(rows=5, columns=5, me=VAMPIRES)
    state.set_cell((1, 1), (0, 300, 0))
    problems = validate_turn(state, [Move(1, 1, 300, 1, 2)])
    assert any("byte" in p for p in problems)
