"""Replay recorded server games through our simulator.

`tools/probe_server.py` records, for every turn, the board we saw and the moves
we sent.  Applying those moves to that board with our own rules must reproduce
what the server actually did.  The recordings are committed under docs/probes/,
so this runs anywhere, including on Kaggle, without the Windows server.

Only the cells our own moves targeted are compared: the rest of the next
board also reflects the opponent's reply, which we are not simulating here.
"""

import json
from pathlib import Path

import pytest

from vvw.rules import Move
from vvw.simulator import apply_turn
from vvw.state import GameState

PROBES = Path(__file__).resolve().parents[1] / "docs" / "probes"

# The probe maps are all 5 rows by 9 columns; the recordings do not carry
# dimensions, so they are supplied here.
RECORDINGS = {
    "freewin_vampires.json": (5, 9),
    "merge_vampires.json": (5, 9),
    "freewin_werewolves.json": (5, 9),
    "merge_werewolves.json": (5, 9),
}
AVAILABLE = {n: d for n, d in RECORDINGS.items() if (PROBES / n).exists()}


def _state(rows, columns, board, me) -> GameState:
    state = GameState(rows=rows, columns=columns, me=me)
    for x, y, humans, vampires, werewolves in board:
        state.set_cell((x, y), (humans, vampires, werewolves))
    return state


@pytest.mark.parametrize("name", sorted(AVAILABLE))
def test_our_simulator_reproduces_the_server(name):
    rows, columns = AVAILABLE[name]
    recording = json.loads((PROBES / name).read_text(encoding="utf-8"))
    species = recording["assigned_species"]
    turns = recording["turns"]

    compared = 0
    for current, following in zip(turns, turns[1:]):
        moves = [Move(*m) for m in current["sent"]]
        if not moves:
            continue
        before = _state(rows, columns, current["board_before"], species)
        after = apply_turn(before, moves, species)
        expected = _state(rows, columns, following["board_before"], species)

        for target in {m.target for m in moves}:
            assert after.get(target) == expected.get(target), (
                f"{name} turn {current['index']}: cell {target} became "
                f"{after.get(target)} in our simulator but "
                f"{expected.get(target)} on the server"
            )
            compared += 1

    assert compared, f"{name} recorded nothing to compare"


@pytest.mark.skipif(
    "freewin_vampires.json" not in AVAILABLE, reason="probe not recorded"
)
def test_a_deterministic_win_costs_the_attacker_nothing():
    """The server kept all 6 attackers after wiping out 4 defenders."""
    recording = json.loads(
        (PROBES / "freewin_vampires.json").read_text(encoding="utf-8")
    )
    attack = recording["turns"][1]
    assert attack["sent"] == [[4, 2, 6, 5, 2]]
    board = {(x, y): (h, v, w) for x, y, h, v, w in attack["board_before"]}
    assert board[(5, 2)] == (0, 0, 4), "4 werewolves were defending"

    after = {
        (x, y): (h, v, w)
        for x, y, h, v, w in recording["turns"][2]["board_before"]
    }
    assert after[(5, 2)] == (0, 6, 0), "all 6 attackers survived"


@pytest.mark.skipif(
    "merge_vampires.json" not in AVAILABLE, reason="probe not recorded"
)
def test_moves_into_one_cell_merge_before_the_fight():
    """3 + 3 beat 4 outright, which only works if they merged first."""
    recording = json.loads(
        (PROBES / "merge_vampires.json").read_text(encoding="utf-8")
    )
    attack = recording["turns"][1]
    assert attack["sent"] == [[0, 1, 3, 1, 2], [2, 1, 3, 1, 2]]
    board = {(x, y): (h, v, w) for x, y, h, v, w in attack["board_before"]}
    assert board[(1, 2)] == (0, 0, 4), "4 werewolves were defending"

    after = {
        (x, y): (h, v, w)
        for x, y, h, v, w in recording["turns"][2]["board_before"]
    }
    assert after[(1, 2)] == (0, 6, 0), (
        "a clean 6-0 means 3+3 merged to 6 and cleared the 1.5x bar; "
        "two separate 3-vs-4 random battles would almost never look like this"
    )
