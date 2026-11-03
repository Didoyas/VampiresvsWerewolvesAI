"""The client against a mock server that speaks the PDF's protocol.

Every other test exercises a piece. This one runs `main.play` over a real
socket through a whole game, because the failure that costs the most marks is
not a weak move: it is a client that cannot talk to the server at all, which
caps the grade at 10/20 however good the AI is.

The mock server sends exactly what the assignment specifies and nothing else:
SET rows columns, HUM, HME, MAP, then UPD, then END and BYE.
"""

import socket
import threading

import pytest

import main
from vvw.protocol import recv_exactly

# A 10x10 board: two houses, vampires at (1,1), werewolves at (8,8).
ROWS, COLUMNS = 10, 10
HOUSES = [(4, 4), (5, 5)]
CELLS = [(1, 1, 0, 6, 0), (8, 8, 0, 0, 6), (4, 4, 3, 0, 0), (5, 5, 3, 0, 0)]


def _cells_packet(name, cells):
    out = bytearray(name.encode("ascii"))
    out.append(len(cells))
    for cell in cells:
        out.extend(bytes(cell))
    return bytes(out)


class MockServer:
    """Plays the server's side of one game, then says BYE."""

    def __init__(self, updates=1, moving_first=True):
        self.updates = updates
        #: The first player gets an empty first UPD; the second player's
        #: first UPD already carries the opening move.
        self.moving_first = moving_first
        self.name = None
        self.movs = []
        self.error = None
        self._listener = socket.socket()
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(1)
        self.port = self._listener.getsockname()[1]
        self._thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._thread.join(timeout=20)
        self._listener.close()

    def _run(self):
        try:
            conn, _ = self._listener.accept()
            with conn:
                conn.settimeout(15)
                header = recv_exactly(conn, 4)
                assert header[:3] == b"NME", header
                self.name = recv_exactly(conn, header[3]).decode("ascii")

                conn.sendall(b"SET" + bytes([ROWS, COLUMNS]))
                houses = bytearray(b"HUM")
                houses.append(len(HOUSES))
                for x, y in HOUSES:
                    houses.extend(bytes([x, y]))
                conn.sendall(bytes(houses))
                conn.sendall(b"HME" + bytes([1, 1]))
                conn.sendall(_cells_packet("MAP", CELLS))

                for index in range(self.updates):
                    opening = [] if (index or self.moving_first) else [(8, 7, 0, 0, 6)]
                    conn.sendall(_cells_packet("UPD", opening))
                    self.movs.append(self._read_mov(conn))

                conn.sendall(b"END")
                conn.sendall(b"BYE")
        except Exception as error:  # surfaced in the test, not swallowed
            self.error = error

    @staticmethod
    def _read_mov(conn):
        header = recv_exactly(conn, 4)
        assert header[:3] == b"MOV", header
        count = header[3]
        raw = recv_exactly(conn, 5 * count)
        return [tuple(raw[i:i + 5]) for i in range(0, len(raw), 5)]


def _check_moves(moves):
    assert moves, "rule 1: at least one movement per ply"
    for sx, sy, count, tx, ty in moves:
        assert 0 <= sx < COLUMNS and 0 <= sy < ROWS, (sx, sy)
        assert 0 <= tx < COLUMNS and 0 <= ty < ROWS, (tx, ty)
        assert count >= 1, "rule 6: at least one creature"
        assert max(abs(tx - sx), abs(ty - sy)) == 1, "rule 4: one step, 8 directions"


def _run_client(server, policy=None, name="TestAI"):
    from vvw.protocol import Connection

    connection = Connection.connect("127.0.0.1", server.port, timeout=15)
    try:
        main.play(connection, name, policy)
    finally:
        connection.close()


# ---- the heuristic client, which needs no torch ----------------------------


def test_a_whole_game_over_a_real_socket():
    with MockServer(updates=3) as server:
        _run_client(server)
    assert server.error is None, server.error
    assert server.name == "TestAI"
    assert len(server.movs) == 3
    for moves in server.movs:
        _check_moves(moves)


def test_the_client_works_out_which_species_it_is():
    """HME names our cell; whoever occupies it in MAP is us."""
    captured = {}
    original = main.decide

    def spy(state, deadline, policy=None):
        captured["me"] = state.me
        return original(state, deadline, policy)

    with pytest.MonkeyPatch.context() as patch:
        patch.setattr(main, "decide", spy)
        with MockServer(updates=1) as server:
            _run_client(server)
    assert server.error is None, server.error
    assert captured["me"] == "vampires", "HME was (1,1), where the vampires are"


# ---- the policy client -----------------------------------------------------


def test_a_missing_checkpoint_falls_back_instead_of_crashing(tmp_path):
    assert main.load_policy(str(tmp_path / "nothing.pt")) is None


def test_a_corrupt_checkpoint_falls_back_instead_of_crashing(tmp_path):
    bad = tmp_path / "bad.pt"
    bad.write_bytes(b"this is not a checkpoint")
    assert main.load_policy(str(bad)) is None


def test_no_checkpoint_means_no_policy():
    assert main.load_policy(None) is None
    assert main.load_policy("") is None


def test_a_real_checkpoint_plays_a_whole_game(tmp_path):
    torch = pytest.importorskip("torch")

    from config import PRESETS
    from vvw.network import PolicyValueNet

    shape = PRESETS["default"].network
    network = PolicyValueNet(
        channels=shape.channels, blocks=shape.blocks, head_hidden=shape.head_hidden
    )
    path = tmp_path / "best.pt"
    torch.save({"network": network.state_dict(), "step": 1234}, path)

    policy = main.load_policy(str(path))
    assert policy is not None, "an untrained but well-shaped checkpoint must load"

    with MockServer(updates=3) as server:
        _run_client(server, policy)
    assert server.error is None, server.error
    assert len(server.movs) == 3
    for moves in server.movs:
        _check_moves(moves)


def test_a_policy_that_raises_still_produces_a_legal_turn():
    """The heuristic underneath has to carry the turn."""

    def broken(state, species):
        raise RuntimeError("pretend the network blew up")

    with MockServer(updates=2) as server:
        _run_client(server, broken)
    assert server.error is None, server.error
    assert len(server.movs) == 2
    for moves in server.movs:
        _check_moves(moves)


def test_an_illegal_policy_turn_is_rejected_and_replaced():
    from vvw.rules import Move

    def cheats(state, species):
        # Nine cells away in one ply, and from a cell we do not hold.
        return [Move(0, 0, 99, 9, 9)]

    with MockServer(updates=2) as server:
        _run_client(server, cheats)
    assert server.error is None, server.error
    for moves in server.movs:
        _check_moves(moves)


def test_the_policy_is_skipped_when_the_clock_has_nearly_run_out():
    """Below the reserve the heuristic plays, so a slow machine still answers."""
    import time

    from vvw import mapgen
    from vvw.state import VAMPIRES

    called = []

    def policy(state, species):
        called.append(1)
        return None

    state = mapgen.generate(1, rows=10, columns=10)
    state.me = VAMPIRES
    turn = main.decide(state, time.monotonic() + 0.01, policy)
    assert called == [], "there was no time for the policy"
    assert turn, "the heuristic still had to produce a turn"


# ---- choosing the checkpoint without a flag --------------------------------


class _Args:
    def __init__(self, checkpoint=None, no_policy=False):
        self.checkpoint = checkpoint
        self.no_policy = no_policy


def test_no_policy_wins_over_everything():
    assert main.resolve_checkpoint(_Args(checkpoint="somewhere.pt", no_policy=True)) == ""


def test_an_explicit_checkpoint_is_used_as_given():
    assert main.resolve_checkpoint(_Args(checkpoint="given.pt")) == "given.pt"


def test_best_pt_beside_main_is_found_without_a_flag(tmp_path):
    """The launcher appends only the ip and the port, so a flag cannot be
    relied on: dropping the file in has to be enough to turn the policy on."""
    beside = tmp_path / "best.pt"
    beside.write_bytes(b"placeholder")
    with pytest.MonkeyPatch.context() as patch:
        patch.setattr(main, "DEFAULT_CHECKPOINT", beside)
        assert main.resolve_checkpoint(_Args()) == str(beside)


def test_without_the_file_it_falls_back_to_the_heuristic(tmp_path):
    with pytest.MonkeyPatch.context() as patch:
        patch.setattr(main, "DEFAULT_CHECKPOINT", tmp_path / "absent.pt")
        assert main.resolve_checkpoint(_Args()) == ""


def test_the_play_path_does_not_need_gymnasium():
    """`vvw.env` imports gymnasium; the client has no reason to.

    Training pulls in gymnasium, torch and numpy. On the CTI machines the
    client may have none of them, and the heuristic must still play.
    """
    import subprocess
    import sys
    import textwrap

    program = textwrap.dedent(
        """
        import sys
        class Block:
            def find_module(self, name, path=None):
                if name == "gymnasium" or name.startswith("gymnasium."):
                    return self
            def load_module(self, name):
                raise ImportError("gymnasium is not installed here")
        sys.meta_path.insert(0, Block())
        import main
        from vvw.candidates import observation_for
        assert "gymnasium" not in sys.modules
        print("ok")
        """
    )
    result = subprocess.run(
        [sys.executable, "-c", program],
        capture_output=True,
        text=True,
        cwd=str(__import__("pathlib").Path(__file__).resolve().parents[1]),
    )
    assert result.returncode == 0, result.stderr
    assert "ok" in result.stdout


# ---- the shared move cap ---------------------------------------------------


def _plies_after(exchanges, moving_first):
    """Run a game and report the ply counters the agent was last given."""
    seen = {}
    original = main.decide

    def spy(state, deadline, policy=None):
        seen["plies"] = dict(state.plies)
        return original(state, deadline, policy)

    with pytest.MonkeyPatch.context() as patch:
        patch.setattr(main, "decide", spy)
        with MockServer(updates=exchanges, moving_first=moving_first) as server:
            _run_client(server)
    assert server.error is None, server.error
    return seen["plies"]


def test_moving_first_the_opponent_is_one_ply_behind():
    """`evaluate.turns_remaining` sums both counters because the 200-move cap
    is shared. Counting only our own made the 90th turn look like the 50th."""
    plies = _plies_after(4, moving_first=True)
    assert plies["vampires"] == 3, plies
    assert plies["werewolves"] == 3, plies


def test_moving_second_both_counters_are_level():
    plies = _plies_after(4, moving_first=False)
    assert plies["vampires"] == 3, plies
    assert plies["werewolves"] == 4, plies


def test_the_counters_track_the_real_total_not_half_of_it():
    """The bug this guards: the sum used to be half the plies actually
    played, so the agent read the endgame as the opening."""
    from vvw.evaluate import turns_remaining
    from vvw.state import GameState

    plies = _plies_after(6, moving_first=True)
    state = GameState(rows=10, columns=10)
    state.plies = plies
    honest = GameState(rows=10, columns=10)
    honest.plies = {"vampires": 5, "werewolves": 0}
    assert turns_remaining(state) < turns_remaining(honest), (
        "both sides counted must leave fewer turns than counting one"
    )
