"""Tournament client.

The CTI machines append the server's ip and port to whatever else is on the
command line, so the last two arguments are read positionally:

    python main.py 127.0.0.1 5555                 # best.pt beside this file
    python main.py --checkpoint other.pt 127.0.0.1 5555
    python main.py --no-policy 127.0.0.1 5555     # heuristic only

The tournament launcher appends only the ip and the port, so the checkpoint
cannot be assumed to arrive as a flag: with no --checkpoint this looks for
`best.pt` next to this file and plays it if it is there. Drop the file in to
turn the policy on, pass --no-policy to turn it off.

Without a policy it plays the heuristic, which needs neither torch nor
gymnasium. With one, the policy picks among the same candidate turns it was
trained on and the heuristic becomes the fallback underneath it.

This file stays a pure client.  Training lives in train.py and must not be
started from here.

Everything it prints goes to stderr, so a redirected stdout can never end up
mixed with protocol traffic.
"""

from __future__ import annotations

import argparse
import sys
import time
import traceback
from pathlib import Path
from typing import List

from vvw.agents import heuristic, naive
from vvw.protocol import Connection, ConnectionClosed, ProtocolError
from vvw.rules import Move, validate_turn
from vvw.state import GameState, VAMPIRES, WEREWOLVES, opponent_of

AI_NAME = "NightShift"

#: Looked for beside this file when no --checkpoint is given.
DEFAULT_CHECKPOINT = Path(__file__).resolve().parent / "best.pt"

# The server allows 2 s per move.  Stop well before that so validation,
# serialisation and the network still fit.
TURN_BUDGET_SECONDS = 1.5

# Time held back for the heuristic fallback.  Measured on the tournament-sized
# maps, a policy decision costs 6-28 ms and the heuristic 0-3 ms, so this is
# about fifty times the headroom either needs; it exists so that a machine far
# slower than this one still answers within the server's two seconds rather
# than forfeiting the turn.
POLICY_RESERVE_SECONDS = 0.5


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def parse_args(argv: List[str]) -> argparse.Namespace:
    """ip and port are the last two arguments, whatever precedes them."""
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--name", default=AI_NAME)
    parser.add_argument(
        "--checkpoint",
        default=None,
        help=f"a trained checkpoint to play with (default: {DEFAULT_CHECKPOINT.name} "
        "beside main.py, if it exists)",
    )
    parser.add_argument(
        "--no-policy",
        action="store_true",
        help="ignore any checkpoint and play the heuristic",
    )
    parser.add_argument("--help", action="help")
    known, _ = parser.parse_known_args(argv)

    positional = [a for a in argv if not a.startswith("-")]
    if len(positional) < 2:
        parser.error("expected the server ip and port as the last two arguments")
    known.ip = positional[-2]
    try:
        known.port = int(positional[-1])
    except ValueError:
        parser.error(f"port {positional[-1]!r} is not a number")
    return known


def resolve_checkpoint(args) -> str:
    """Which checkpoint to play, if any.

    The launcher appends the ip and the port and nothing else, so relying on
    a --checkpoint flag would mean the policy never loads on the day. An
    explicit flag still wins; otherwise the presence of the file decides.
    """
    if args.no_policy:
        return ""
    if args.checkpoint:
        return args.checkpoint
    if DEFAULT_CHECKPOINT.exists():
        return str(DEFAULT_CHECKPOINT)
    log(f"[policy] no {DEFAULT_CHECKPOINT.name} beside main.py; playing the heuristic")
    return ""


def load_policy(path: str):
    """Build the network agent, or return None and keep playing the heuristic.

    Every failure here is survivable and none of them may raise: a missing
    file, a torch that is not installed on the CTI machine, a checkpoint
    saved from a different network shape.  A client that cannot play caps the
    grade at 10/20, so the policy is an upgrade on the heuristic and never a
    dependency of it.

    The network is warmed up on a throwaway board before the game starts.
    The first forward pass costs about 115 ms against 6-28 ms afterwards, and
    that first one would otherwise land on a real turn.
    """
    if not path:
        return None
    try:
        import torch

        from config import PRESETS
        from vvw import mapgen
        from vvw.agents.policy import PolicyAgent
        from vvw.network import PolicyValueNet

        shape = PRESETS["default"].network
        network = PolicyValueNet(
            channels=shape.channels,
            blocks=shape.blocks,
            head_hidden=shape.head_hidden,
        )
        checkpoint = torch.load(path, map_location="cpu")
        network.load_state_dict(checkpoint["network"])
        network.eval()
        agent = PolicyAgent(network, deterministic=True, seed=0)

        started = time.monotonic()
        agent(mapgen.generate(0, rows=10, columns=10), VAMPIRES)
        trained_to = checkpoint.get("step", "unknown")
        log(
            f"[policy] {path} (trained to step {trained_to}) ready in "
            f"{time.monotonic() - started:.2f}s"
        )
        return agent
    except Exception:
        log("[warn] no policy, playing the heuristic:\n" + traceback.format_exc())
        return None


def decide(state: GameState, deadline: float, policy=None) -> List[Move]:
    """Pick this turn's moves, always returning something legal.

    The policy plays when there is one and there is time; the heuristic is
    the net beneath it, the naive agent the net beneath that, and the
    single-move fallback below the last one.  Whatever happens, this returns
    a validated legal turn before `deadline`.
    """
    turn = []

    if policy is not None and time.monotonic() < deadline - POLICY_RESERVE_SECONDS:
        try:
            candidate = policy(state, state.me)
        except Exception:
            log("[bug] the policy raised:\n" + traceback.format_exc())
            candidate = None
        if candidate:
            problems = validate_turn(state, candidate)
            if problems:
                log(f"[bug] the policy proposed an illegal turn: {problems}")
            else:
                return candidate
    for name, agent in (("heuristic", heuristic.choose_turn), ("naive", naive.choose_turn)):
        try:
            candidate = agent(state, state.me)
        except Exception:
            log(f"[bug] {name} agent raised:\n" + traceback.format_exc())
            continue
        problems = validate_turn(state, candidate)
        if problems:
            log(f"[bug] {name} agent proposed an illegal turn: {problems}")
            continue
        if candidate:
            turn = candidate
            break

    if not turn:
        # Last resort so that rule 1 is still satisfied.
        groups = state.groups(state.me)
        if groups:
            (x, y), count = max(groups, key=lambda g: g[1])
            for nx, ny in state.neighbours((x, y)):
                turn = [Move(x, y, count, nx, ny)]
                break
    if time.monotonic() > deadline:
        log("[warn] decision ran past its budget")
    return turn


def play(connection: Connection, name: str, policy=None) -> None:
    connection.send_nme(name)
    state = GameState()
    games = 0
    updates = 0

    while True:
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
            # Whichever species occupies our starting cell is ours.
            humans, vampires, werewolves = state.get(state.home or (0, 0))
            state.me = VAMPIRES if vampires >= werewolves else WEREWOLVES
            games += 1
            updates = 0
            log(
                f"[game {games}] {state.rows}x{state.columns} "
                f"as {state.me}, {len(state.houses)} houses"
            )

        elif message.name == "UPD":
            started = time.monotonic()
            deadline = started + TURN_BUDGET_SECONDS
            state.apply_cell_updates(message.payload.cells)

            # Both counters have to advance, because the move cap is shared
            # and `evaluate.turns_remaining` sums them. Counting only our own
            # plies told the agent it had 75 turns left on its 50th and put
            # 0.275 instead of 0.050 in the fifth scalar on its 90th.
            #
            # Training never had this problem: there both sides move through
            # `apply_turn`, which counts whoever acts. It was the client alone
            # that drifted, so this is a train/serve mismatch rather than a
            # training bug. Measured over 2,210 decisions on the test maps it
            # changed 2% of the policy's turns and none of the heuristic's --
            # small, but free to get right.
            #
            # The PDF says the first player receives MAP and then a UPD with
            # no changes, so an empty first update means the opponent has not
            # moved yet and a full one means it already has.
            updates += 1
            them = opponent_of(state.me)
            if updates == 1:
                state.plies[them] = 0 if not message.payload.cells else 1
            else:
                state.plies[them] = state.plies.get(them, 0) + 1

            turn = decide(state, deadline, policy)
            connection.send_mov([m.as_wire() for m in turn])
            state.plies[state.me] = state.plies.get(state.me, 0) + 1
            elapsed = time.monotonic() - started
            if elapsed > TURN_BUDGET_SECONDS:
                log(f"[warn] turn took {elapsed:.3f}s")

        elif message.name == "END":
            log(f"[game {games}] over; waiting for the next one")
            state.reset_board()
            updates = 0

        elif message.name == "BYE":
            log("server said BYE")
            return


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    connection = None
    try:
        # Before the socket, so a broken checkpoint cannot cost us the first
        # turn, and so the warm-up happens off the clock.
        policy = load_policy(resolve_checkpoint(args))
        connection = Connection.connect(args.ip, args.port)
        play(connection, args.name, policy)
        return 0
    except ConnectionClosed:
        log("the server closed the connection")
        return 0
    except (OSError, ProtocolError) as error:
        log(f"connection problem: {error}")
        return 1
    except Exception:
        log("unhandled error:\n" + traceback.format_exc())
        return 1
    finally:
        if connection is not None:
            connection.close()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
