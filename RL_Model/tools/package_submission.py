"""Build the folder to hand in, then prove it plays.

    python tools/package_submission.py --checkpoint path/to/best.pt --out submission

The assignment says to post the source code and that the AI runs on the CTI
machines, so what is handed in is a folder, not a checkpoint: the weights are
useless without the code that builds the candidate turns they choose between.

The checkpoint is copied in as `best.pt` beside `main.py`, which is where the
client looks when no flag is given. That matters because the launcher appends
only the ip and the port:

    python main.py 123.123.3.5 6666

The check at the end is the point of this tool. It starts a mock server that
speaks the protocol from the assignment PDF, launches `main.py` as a separate
process exactly as the launcher would, and reports whether a legal MOV came
back in time. A client that cannot talk to the server caps the grade at
10/20, so this is worth running on the machine that will do the playing.
"""

from __future__ import annotations

import argparse
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

ROOT = Path(__file__).resolve().parents[1]

#: Left out of the handed-in folder: caches and training output, nothing the
#: grader or the client needs.
SKIP_DIRS = {"__pycache__", ".pytest_cache", ".ipynb_checkpoints", "runs"}
SKIP_SUFFIXES = {".pyc", ".pt", ".log", ".zip"}

#: Everything the client touches while playing. `vvw/env.py` is deliberately
#: absent: it imports gymnasium, and nothing on the play path needs it.
RUNTIME_MODULES = [
    "main.py",
    "config.py",
    "vvw/__init__.py",
    "vvw/protocol.py",
    "vvw/state.py",
    "vvw/rules.py",
    "vvw/simulator.py",
    "vvw/evaluate.py",
    "vvw/features.py",
    "vvw/mapgen.py",
    "vvw/candidates.py",
    "vvw/network.py",
    "vvw/agents/__init__.py",
    "vvw/agents/heuristic.py",
    "vvw/agents/naive.py",
    "vvw/agents/random_legal.py",
    "vvw/agents/policy.py",
]

ROWS, COLUMNS = 10, 10
CELLS = [(1, 1, 0, 6, 0), (8, 8, 0, 0, 6), (4, 4, 3, 0, 0), (5, 5, 3, 0, 0)]


def copy_tree(out: Path) -> int:
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    copied = 0
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(ROOT)
        if any(part in SKIP_DIRS for part in relative.parts):
            continue
        if path.suffix in SKIP_SUFFIXES:
            continue
        target = out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        copied += 1
    return copied


def recv_exactly(sock: socket.socket, n: int) -> bytes:
    chunks, got = [], 0
    while got < n:
        chunk = sock.recv(n - got)
        if not chunk:
            raise ConnectionError(f"closed after {got}/{n} bytes")
        chunks.append(chunk)
        got += len(chunk)
    return b"".join(chunks)


class MockServer:
    """The server's side of a short game, as the PDF specifies it."""

    def __init__(self, turns: int = 4):
        self.turns = turns
        self.name: Optional[str] = None
        self.movs: List[List[tuple]] = []
        self.timings: List[float] = []
        self.error: Optional[Exception] = None
        self._listener = socket.socket()
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(1)
        self.port = self._listener.getsockname()[1]
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self._thread.start()
        return self

    def join(self, timeout: float = 60.0):
        self._thread.join(timeout=timeout)
        self._listener.close()

    def _run(self):
        try:
            self._listener.settimeout(45)
            conn, _ = self._listener.accept()
            with conn:
                conn.settimeout(30)
                header = recv_exactly(conn, 4)
                if header[:3] != b"NME":
                    raise AssertionError(f"expected NME, got {header[:3]!r}")
                self.name = recv_exactly(conn, header[3]).decode("ascii")

                conn.sendall(b"SET" + bytes([ROWS, COLUMNS]))
                conn.sendall(b"HUM" + bytes([2, 4, 4, 5, 5]))
                conn.sendall(b"HME" + bytes([1, 1]))
                packet = bytearray(b"MAP")
                packet.append(len(CELLS))
                for cell in CELLS:
                    packet.extend(bytes(cell))
                conn.sendall(bytes(packet))

                for _ in range(self.turns):
                    conn.sendall(b"UPD" + bytes([0]))
                    started = time.monotonic()
                    head = recv_exactly(conn, 4)
                    if head[:3] != b"MOV":
                        raise AssertionError(f"expected MOV, got {head[:3]!r}")
                    raw = recv_exactly(conn, 5 * head[3])
                    self.timings.append(time.monotonic() - started)
                    self.movs.append(
                        [tuple(raw[i:i + 5]) for i in range(0, len(raw), 5)]
                    )
                conn.sendall(b"END")
                conn.sendall(b"BYE")
        except Exception as error:
            self.error = error


def illegal(moves) -> Optional[str]:
    if not moves:
        return "empty MOV; rule 1 wants at least one movement"
    for sx, sy, count, tx, ty in moves:
        if not (0 <= sx < COLUMNS and 0 <= sy < ROWS):
            return f"source ({sx},{sy}) is off the board"
        if not (0 <= tx < COLUMNS and 0 <= ty < ROWS):
            return f"target ({tx},{ty}) is off the board"
        if count < 1:
            return "moving zero creatures breaks rule 6"
        if max(abs(tx - sx), abs(ty - sy)) != 1:
            return f"({sx},{sy})->({tx},{ty}) is not one step"
    return None


def check(out: Path, extra: List[str]) -> bool:
    print("\n-- playing a game against a mock server --")
    server = MockServer().start()
    command = [sys.executable, "main.py"] + extra + ["127.0.0.1", str(server.port)]
    print("  " + " ".join(command))
    process = subprocess.run(
        command, cwd=str(out), capture_output=True, text=True, timeout=120
    )
    server.join()

    for line in (process.stderr or "").strip().splitlines():
        print("  | " + line)
    if server.error is not None:
        print(f"  FAIL the server never completed the game: {server.error}")
        return False
    if process.returncode != 0:
        print(f"  FAIL the client exited with {process.returncode}")
        return False

    print(f"  name sent: {server.name}")
    for index, moves in enumerate(server.movs, 1):
        problem = illegal(moves)
        status = "illegal: " + problem if problem else f"{len(moves)} move(s)"
        print(f"  turn {index}: {server.timings[index-1]*1000:6.0f} ms  {status}")
        if problem:
            return False
    worst = max(server.timings) if server.timings else 0.0
    if worst > 2.0:
        print(f"  FAIL slowest turn {worst:.2f}s is over the server's 2 s limit")
        return False
    print(f"  OK   {len(server.movs)} legal turns, slowest {worst*1000:.0f} ms of 2000 ms")
    return True


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", help="copied in as best.pt beside main.py")
    parser.add_argument("--out", default="submission")
    parser.add_argument(
        "--no-policy",
        action="store_true",
        help="check the heuristic instead of the checkpoint",
    )
    args = parser.parse_args(argv)

    out = Path(args.out).resolve()
    copied = copy_tree(out)
    print(f"{copied} files copied to {out}")

    missing = [m for m in RUNTIME_MODULES if not (out / m).exists()]
    if missing:
        print("MISSING runtime modules: " + ", ".join(missing))
        return 1
    print(f"all {len(RUNTIME_MODULES)} runtime modules present")

    if args.checkpoint:
        shutil.copy2(args.checkpoint, out / "best.pt")
        size = (out / "best.pt").stat().st_size / 1024
        print(f"checkpoint copied in as best.pt ({size:.0f} KB)")
    else:
        print("no checkpoint given; the folder will play the heuristic")

    ok = check(out, ["--no-policy"] if args.no_policy else [])
    print("\n" + ("READY" if ok else "NOT READY") + f": {out}")
    print("Launch on the day:  python main.py <ip> <port>")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
