"""Applying turns: movement, merging, conversion and battles."""

import random

import pytest

from vvw.agents import naive
from vvw.rules import Move, Unknowns, validate_turn
from vvw.simulator import apply_turn, is_terminal, winner
from vvw.state import GameState, VAMPIRES, WEREWOLVES


def board() -> GameState:
    state = GameState(rows=6, columns=6, me=VAMPIRES)
    state.set_cell((1, 1), (0, 10, 0))
    state.set_cell((4, 4), (0, 0, 8))
    state.set_cell((1, 3), (4, 0, 0))
    state.houses.add((1, 3))
    return state


# ------------------------------------------------------------------ movement


def test_moving_the_whole_group_empties_the_source():
    after = apply_turn(board(), [Move(1, 1, 10, 2, 1)])
    assert after.get((1, 1)) == (0, 0, 0)
    assert after.get((2, 1)) == (0, 10, 0)


def test_moving_part_of_a_group_leaves_the_rest_behind():
    after = apply_turn(board(), [Move(1, 1, 4, 2, 1)])
    assert after.get((1, 1)) == (0, 6, 0)
    assert after.get((2, 1)) == (0, 4, 0)


def test_the_original_state_is_not_modified():
    state = board()
    apply_turn(state, [Move(1, 1, 10, 2, 1)])
    assert state.get((1, 1)) == (0, 10, 0)


def test_an_illegal_turn_is_refused():
    with pytest.raises(ValueError):
        apply_turn(board(), [Move(1, 1, 99, 2, 1)])


def test_splitting_then_regrouping_conserves_the_population():
    state = board()
    after = apply_turn(state, [Move(1, 1, 4, 0, 1), Move(1, 1, 6, 2, 1)])
    assert after.population(VAMPIRES) == 10
    # Both halves walk into the same empty cell next turn and merge.
    after2 = apply_turn(after, [Move(0, 1, 4, 1, 0), Move(2, 1, 6, 1, 0)])
    assert after2.get((1, 0)) == (0, 10, 0)


def test_two_groups_moving_into_one_friendly_cell_merge():
    state = GameState(rows=5, columns=5, me=VAMPIRES)
    state.set_cell((0, 0), (0, 3, 0))
    state.set_cell((2, 0), (0, 4, 0))
    state.set_cell((1, 1), (0, 2, 0))  # stays put, so rule 5 is satisfied
    after = apply_turn(state, [Move(0, 0, 3, 1, 1), Move(2, 0, 4, 1, 1)])
    assert after.get((1, 1)) == (0, 9, 0)


# ---------------------------------------------------------------- conversion


def test_meeting_humans_in_equal_numbers_converts_them_all():
    state = board()
    state.set_cell((1, 2), (0, 4, 0))  # exactly as many as the 4 humans
    after = apply_turn(state, [Move(1, 2, 4, 1, 3)])
    assert after.get((1, 3)) == (0, 8, 0)
    assert after.population(VAMPIRES) == 18


def test_converting_humans_costs_nothing():
    state = board()
    after = apply_turn(state, [Move(1, 1, 10, 1, 2)])
    after = apply_turn(after, [Move(1, 2, 10, 1, 3)])
    assert after.population(VAMPIRES) == 14  # 10 of ours plus 4 converted


def test_attacking_humans_understrength_is_a_random_battle():
    state = board()
    state.set_cell((1, 2), (0, 2, 0))  # fewer than the 4 humans
    rng = random.Random(3)
    after = apply_turn(state, [Move(1, 2, 2, 1, 3)], rng=rng)
    humans, vampires, _ = after.get((1, 3))
    assert (humans > 0) != (vampires > 0)  # exactly one side remains


# ------------------------------------------------------------------- combat


def test_reaching_one_and_a_half_times_wipes_out_the_enemy():
    state = GameState(rows=5, columns=5, me=VAMPIRES)
    state.set_cell((1, 1), (0, 6, 0))
    state.set_cell((1, 2), (0, 0, 4))
    after = apply_turn(state, [Move(1, 1, 6, 1, 2)])
    assert after.get((1, 2)) == (0, 6, 0)
    assert after.population(WEREWOLVES) == 0


def test_attacking_a_much_stronger_group_loses_everyone():
    state = GameState(rows=5, columns=5, me=VAMPIRES)
    state.set_cell((1, 1), (0, 4, 0))
    state.set_cell((1, 2), (0, 0, 6))
    after = apply_turn(state, [Move(1, 1, 4, 1, 2)])
    assert after.get((1, 2)) == (0, 0, 6)
    assert after.population(VAMPIRES) == 0


def test_two_groups_merge_before_fighting():
    # 3 + 3 = 6 reaches 1.5x against 4, while 3 alone would not.
    state = GameState(rows=5, columns=5, me=VAMPIRES)
    state.set_cell((0, 1), (0, 3, 0))
    state.set_cell((2, 1), (0, 3, 0))
    state.set_cell((1, 2), (0, 0, 4))
    after = apply_turn(state, [Move(0, 1, 3, 1, 2), Move(2, 1, 3, 1, 2)])
    assert after.get((1, 2)) == (0, 6, 0)


def test_a_battle_never_leaves_both_sides_on_one_cell():
    rng = random.Random(11)
    for _ in range(100):
        state = GameState(rows=5, columns=5, me=VAMPIRES)
        state.set_cell((1, 1), (0, 5, 0))
        state.set_cell((1, 2), (0, 0, 4))
        after = apply_turn(state, [Move(1, 1, 5, 1, 2)], rng=rng)
        _, vampires, werewolves = after.get((1, 2))
        assert vampires == 0 or werewolves == 0


# ---------------------------------------------------------------- terminal


def test_a_game_ends_when_a_species_is_wiped_out():
    state = GameState(rows=5, columns=5, me=VAMPIRES)
    state.set_cell((0, 0), (0, 5, 0))
    assert is_terminal(state)
    assert winner(state) == VAMPIRES


def test_a_game_is_not_over_while_both_sides_have_creatures():
    assert not is_terminal(board())


def test_the_bigger_population_wins():
    state = GameState(rows=5, columns=5)
    state.set_cell((0, 0), (0, 5, 0))
    state.set_cell((4, 4), (0, 0, 4))
    assert winner(state) == VAMPIRES


def test_humans_do_not_count_towards_winning():
    state = GameState(rows=5, columns=5)
    state.set_cell((0, 0), (0, 4, 0))
    state.set_cell((4, 4), (0, 0, 4))
    state.set_cell((2, 2), (50, 0, 0))
    assert winner(state, first_mover=None) is None  # a tie, not a human landslide


def test_the_move_cap_is_shared_so_each_player_gets_about_a_hundred():
    """MaxPlays=200 counts the whole game, not each player separately."""
    state = GameState(rows=5, columns=5)
    state.set_cell((0, 0), (0, 5, 0))
    state.set_cell((4, 4), (0, 0, 5))

    state.plies = {VAMPIRES: 99, WEREWOLVES: 99}  # 198 turns played
    assert not is_terminal(state)

    state.plies = {VAMPIRES: 100, WEREWOLVES: 100}  # 200 turns played
    assert is_terminal(state)


def test_the_cap_can_be_switched_to_per_player():
    """The reading is a flag, so a server probe can overturn it in one place."""
    per_player = Unknowns(max_plays_counts_per_player=True, max_plays=200)
    state = GameState(rows=5, columns=5)
    state.set_cell((0, 0), (0, 5, 0))
    state.set_cell((4, 4), (0, 0, 5))

    state.plies = {VAMPIRES: 100, WEREWOLVES: 100}
    assert not is_terminal(state, per_player)

    state.plies = {VAMPIRES: 200, WEREWOLVES: 199}
    assert is_terminal(state, per_player)


def test_equal_creature_counts_are_a_genuine_draw():
    """No tie-break: at either cap, equal counts means nobody won.

    A draw is therefore a result an opponent can deliberately play for when
    it is level and cannot win, which the agent has to expect.
    """
    state = GameState(rows=5, columns=5)
    state.set_cell((0, 0), (0, 4, 0))
    state.set_cell((4, 4), (0, 0, 4))
    assert winner(state) is None
    assert winner(state, first_mover=VAMPIRES) is None


def test_one_creature_ahead_is_enough_to_win():
    state = GameState(rows=5, columns=5)
    state.set_cell((0, 0), (0, 5, 0))
    state.set_cell((4, 4), (0, 0, 4))
    assert winner(state) == VAMPIRES


# ------------------------------------------------------------- naive agent


def test_the_naive_agent_always_proposes_a_legal_turn():
    rng = random.Random(5)
    for _ in range(300):
        state = _random_state(rng)
        if not state.groups(state.me):
            continue
        turn = naive.choose_turn(state)
        assert turn, "the agent must move at least once (rule 1)"
        assert validate_turn(state, turn) == []


def test_the_naive_agent_takes_a_free_conversion():
    state = GameState(rows=5, columns=5, me=VAMPIRES)
    state.set_cell((1, 1), (0, 6, 0))
    state.set_cell((1, 2), (5, 0, 0))
    turn = naive.choose_turn(state)
    assert turn[0].target == (1, 2)


def test_the_naive_agent_declines_a_fight_it_would_lose():
    state = GameState(rows=5, columns=5, me=VAMPIRES)
    state.set_cell((1, 1), (0, 3, 0))
    state.set_cell((1, 2), (0, 0, 20))  # far too strong
    turn = naive.choose_turn(state)
    assert turn[0].target != (1, 2)


def _random_state(rng: random.Random) -> GameState:
    rows, columns = rng.randint(3, 10), rng.randint(3, 10)
    state = GameState(rows=rows, columns=columns, me=VAMPIRES)
    taken = set()
    for _ in range(rng.randint(1, 8)):
        cell = (rng.randrange(columns), rng.randrange(rows))
        if cell in taken:
            continue
        taken.add(cell)
        kind = rng.choice(["humans", "vampires", "werewolves"])
        count = rng.randint(1, 30)
        state.set_cell(
            cell,
            (
                count if kind == "humans" else 0,
                count if kind == "vampires" else 0,
                count if kind == "werewolves" else 0,
            ),
        )
    return state
