"""Binary TCP protocol for the Vampires VS Werewolves game server.

Wire format (see Project_description.pdf, "Server communication protocol"):
  every command is a 3-byte ASCII name, then a fixed payload; every number is
  one unsigned byte.

    server -> player   SET n m                     n = rows, m = columns
                       HUM k  then k x (x, y)
                       HME x y
                       MAP t  then t x (x, y, humans, vampires, werewolves)
                       UPD t  then t x (x, y, humans, vampires, werewolves)
                       END                         clear the game, keep the socket
                       BYE                         the server is closing

    player -> server   NME len  then len ASCII bytes
                       MOV k    then k x (sx, sy, count, tx, ty)

Coordinates are (x, y) with x the column and y the row, zero-based, origin at
the top left.  SET gives rows first and columns second, which is the opposite
order, so the two are kept apart everywhere in this file.
"""

from __future__ import annotations

import socket
from dataclasses import dataclass, field
from typing import List, Tuple

Coord = Tuple[int, int]
Cell = Tuple[int, int, int, int, int]  # x, y, humans, vampires, werewolves


class ProtocolError(Exception):
    """The peer sent something that is not valid in the current state."""


class ConnectionClosed(Exception):
    """The socket reached EOF while we were expecting more bytes."""


@dataclass
class Message:
    """One decoded command.  `payload` depends on `name`."""

    name: str
    payload: object = None


@dataclass
class SetPayload:
    rows: int
    columns: int


@dataclass
class HumPayload:
    houses: List[Coord] = field(default_factory=list)


@dataclass
class HmePayload:
    x: int
    y: int


@dataclass
class CellsPayload:
    """Used by both MAP and UPD; the server sends whole cell contents."""

    cells: List[Cell] = field(default_factory=list)


SERVER_COMMANDS = {"SET", "HUM", "HME", "MAP", "UPD", "END", "BYE"}


def recv_exactly(sock: socket.socket, n: int) -> bytes:
    """Read exactly `n` bytes.

    TCP is a byte stream: one recv() may return fewer bytes than asked for, so
    every read in this file goes through here.  The skeleton client shipped
    with the assignment used `recv(1 - len(data))`, which only ever read a
    single byte and would raise for n > 1.
    """
    if n == 0:
        return b""
    chunks = []
    got = 0
    while got < n:
        chunk = sock.recv(n - got)
        if not chunk:
            raise ConnectionClosed(f"socket closed after {got}/{n} bytes")
        chunks.append(chunk)
        got += len(chunk)
    return b"".join(chunks)


class Connection:
    """A framed view of the socket.  Does no game logic."""

    def __init__(self, sock: socket.socket):
        self._sock = sock

    @classmethod
    def connect(cls, ip: str, port: int, timeout: float | None = None) -> "Connection":
        sock = socket.create_connection((ip, port), timeout=timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return cls(sock)

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass

    # ---- reading ---------------------------------------------------------

    def read_message(self) -> Message:
        name = recv_exactly(self._sock, 3).decode("ascii", errors="replace")
        if name not in SERVER_COMMANDS:
            raise ProtocolError(f"unknown command {name!r}")

        if name == "SET":
            rows, columns = recv_exactly(self._sock, 2)
            return Message(name, SetPayload(rows=rows, columns=columns))

        if name == "HUM":
            count = recv_exactly(self._sock, 1)[0]
            raw = recv_exactly(self._sock, 2 * count)
            houses = [(raw[i], raw[i + 1]) for i in range(0, len(raw), 2)]
            return Message(name, HumPayload(houses=houses))

        if name == "HME":
            x, y = recv_exactly(self._sock, 2)
            return Message(name, HmePayload(x=x, y=y))

        if name in ("MAP", "UPD"):
            count = recv_exactly(self._sock, 1)[0]
            raw = recv_exactly(self._sock, 5 * count)
            cells = [
                (raw[i], raw[i + 1], raw[i + 2], raw[i + 3], raw[i + 4])
                for i in range(0, len(raw), 5)
            ]
            return Message(name, CellsPayload(cells=cells))

        # END and BYE carry no payload.
        return Message(name)

    # ---- writing ---------------------------------------------------------

    def send_nme(self, name: str) -> None:
        encoded = name.encode("ascii")
        if not 1 <= len(encoded) <= 255:
            raise ValueError(f"name length {len(encoded)} out of range")
        self._sock.sendall(b"NME" + bytes([len(encoded)]) + encoded)

    def send_mov(self, moves) -> None:
        """Send one MOV.  `moves` is a sequence of (sx, sy, count, tx, ty)."""
        self._sock.sendall(encode_mov(moves))


def encode_mov(moves) -> bytes:
    """Serialise a MOV packet, rejecting anything a byte cannot hold.

    Kept separate from Connection so tests can check the bytes without a
    socket, and so the agent can validate a plan before committing to it.
    """
    moves = list(moves)
    if not 0 <= len(moves) <= 255:
        raise ValueError(f"move count {len(moves)} out of range")
    out = bytearray(b"MOV")
    out.append(len(moves))
    for move in moves:
        if len(move) != 5:
            raise ValueError(f"move {move!r} must have 5 fields")
        for value in move:
            if not isinstance(value, int) or not 0 <= value <= 255:
                raise ValueError(f"move field {value!r} does not fit in a byte")
        out.extend(bytes(move))
    return bytes(out)
