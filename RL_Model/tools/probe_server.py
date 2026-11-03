"""Scripted client for settling rules against the real server.

Each probe scenario fixes a map and a move script for both sides, so the
resulting UPD is the server's own answer to one question.  Run one process per
side against a server with PlayersCount=2:

    python tools/probe_server.py --scenario merge --role vampires 127.0.0.1 5555
    python tools/probe_server.py --scenario merge --role werewolves 127.0.0.1 5555

Every UPD and every move we send is appended to a JSON file under
docs/probes/, which doubles as the golden fixture for tests that cannot reach
a Windows server.

Once the script runs out, the side shuffles one spare group between two empty
cells so it keeps satisfying rule 1 without disturbing the experiment.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from vvw.protocol import Connection, ConnectionClosed  # noqa: E402
from vvw.rules import Move, validate_turn  # noqa: E402
from vvw.state import GameState, VAMPIRES, WEREWOLVES  # noqa: E402

PROBE_DIR = Path(__file__).resolve().parents[1] / "docs" / "probes"

# A scenario is a map plus, for each side, the turns to play in order.
# Written as (sx, sy, count, tx, ty).
SCENARIOS: Dict[str, Dict] = {
    # Does a deterministic 1.5x win cost the attacker anything?
    # Werewolves detach one creature so the game survives the kill; then
    # 6 vampires take the remaining 4, where 2*6 >= 3*4 is deterministic.
    # (5,2) ending with 6 vampires means the win is free.
    "freewin": {
        "map": "probe_freewin.xml",
        VAMPIRES: [[(3, 1, 6, 4, 2)], [(4, 2, 6, 5, 2)]],
        WEREWOLVES: [[(5, 2, 2, 6, 3)]],
    },
    # Do several moves into one cell merge before the fight?
    # Turn 1 splits 6 into 3 + 3; turn 2 sends both into the same enemy cell.
    # Merged, 6 vs 4 is a deterministic kill. Resolved separately, each 3 vs 4
    # is a random battle and werewolves usually survive.
    "merge": {
        "map": "probe_merge.xml",
        VAMPIRES: [
            [(1, 0, 3, 0, 1), (1, 0, 3, 2, 1)],
            [(0, 1, 3, 1, 2), (2, 1, 3, 1, 2)],
        ],
        WEREWOLVES: [[(1, 2, 2, 0, 3)]],
    },
}


def shuffle_move(state: GameState, species: str) -> List[Move]:
    """A harmless legal move: step a spare group onto an empty cell.

    Prefers the smallest group so the scenario's main force stays put, and
    refuses any occupied destination so nothing is fought by accident.
    """
    groups = sorted(state.groups(species), key=lambda g: g[1])
    for (x, y), count in groups:
        for nx, ny in state.neighbours((x, y)):
            if state.get((nx, ny)) == (0, 0, 0):
                return [Move(x, y, count, nx, ny)]
    # Everything is boxed in; fall back to any legal step.
    for (x, y), count in groups:
        for nx, ny in state.neighbours((x, y)):
            return [Move(x, y, count, nx, ny)]
    return []


def run(scenario_name: str, role: str, ip: str, port: int, max_turns: int) -> int:
    scenario = SCENARIOS[scenario_name]
    script = [[Move(*m) for m in turn] for turn in scenario[role]]

    PROBE_DIR.mkdir(parents=True, exist_ok=True)
    log_path = PROBE_DIR / f"{scenario_name}_{role}.json"
    record: Dict = {"scenario": scenario_name, "role": role, "turns": []}

    connection = Connection.connect(ip, port, timeout=30)
    state = GameState()
    turn_index = 0

    try:
        connection.send_nme(f"probe-{role[:4]}")
        while turn_index < max_turns:
            message = connection.read_message()

            if message.name == "SET":
                state.reset_board()
                state.rows = message.payload.rows
                state.columns = message.payload.columns
            elif message.name == "HUM":
                state.houses = set(message.payload.houses)
            elif message.name == "HME":
                state.home = (message.payload.x, message.payload.y)
            elif message.name == "MAP":
                state.apply_cell_updates(message.payload.cells)
                _, vampires, werewolves = state.get(state.home or (0, 0))
                state.me = VAMPIRES if vampires >= werewolves else WEREWOLVES
                record["assigned_species"] = state.me
                record["initial_board"] = _snapshot(state)
                if state.me != role:
                    print(
                        f"[probe] server made us {state.me}, not {role}; "
                        "swap the two processes",
                        file=sys.stderr,
                    )
                    return 2
            elif message.name == "UPD":
                state.apply_cell_updates(message.payload.cells)
                before = _snapshot(state)

                turn = script[turn_index] if turn_index < len(script) else []
                if turn and validate_turn(state, turn, state.me):
                    print(
                        f"[probe] scripted turn {turn_index} is illegal here: "
                        f"{validate_turn(state, turn, state.me)}",
                        file=sys.stderr,
                    )
                    turn = []
                if not turn:
                    turn = shuffle_move(state, state.me)

                connection.send_mov([m.as_wire() for m in turn])
                record["turns"].append(
                    {
                        "index": turn_index,
                        "board_before": before,
                        "sent": [list(m) for m in turn],
                        "scripted": turn_index < len(script),
                    }
                )
                turn_index += 1
            elif message.name == "END":
                record["ended"] = True
                break
            elif message.name == "BYE":
                record["ended"] = True
                break
    except ConnectionClosed:
        record["ended"] = "socket closed"
    finally:
        connection.close()
        record["final_board"] = _snapshot(state)
        log_path.write_text(json.dumps(record, indent=2), encoding="utf-8")
        print(f"[probe] wrote {log_path}", file=sys.stderr)

    return 0


def _snapshot(state: GameState) -> List[List[int]]:
    """Occupied cells as sorted [x, y, humans, vampires, werewolves] rows."""
    return [
        [x, y, h, v, w] for (x, y), (h, v, w) in sorted(state.cells.items())
    ]


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", required=True, choices=sorted(SCENARIOS))
    parser.add_argument("--role", required=True, choices=[VAMPIRES, WEREWOLVES])
    parser.add_argument("--max-turns", type=int, default=12)
    parser.add_argument("ip")
    parser.add_argument("port", type=int)
    args = parser.parse_args(argv)
    return run(args.scenario, args.role, args.ip, args.port, args.max_turns)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
