"""Protocol framing: exact reads, decoding and MOV serialisation."""

import socket
import threading

import pytest

from vvw.protocol import (
    Connection,
    ConnectionClosed,
    ProtocolError,
    encode_mov,
    recv_exactly,
)


class FakeSocket:
    """Hands out the stream in fixed-size bites to force partial reads."""

    def __init__(self, data: bytes, chunk: int = 1):
        self._data = data
        self._chunk = chunk
        self._at = 0
        self.sent = bytearray()

    def recv(self, size: int) -> bytes:
        take = min(size, self._chunk, len(self._data) - self._at)
        out = self._data[self._at : self._at + take]
        self._at += take
        return out

    def sendall(self, payload: bytes) -> None:
        self.sent.extend(payload)

    def close(self) -> None:
        pass


# ------------------------------------------------------------- recv_exactly


def test_recv_exactly_reassembles_a_split_stream():
    sock = FakeSocket(b"abcdefgh", chunk=3)
    assert recv_exactly(sock, 8) == b"abcdefgh"


def test_recv_exactly_handles_one_byte_at_a_time():
    sock = FakeSocket(b"SET\x0a\x14", chunk=1)
    assert recv_exactly(sock, 5) == b"SET\x0a\x14"


def test_recv_exactly_raises_when_the_stream_ends_early():
    sock = FakeSocket(b"ab", chunk=1)
    with pytest.raises(ConnectionClosed):
        recv_exactly(sock, 4)


def test_recv_exactly_of_zero_bytes_is_a_no_op():
    assert recv_exactly(FakeSocket(b""), 0) == b""


# ------------------------------------------------------------------ decoding


def test_set_carries_rows_then_columns():
    # SET gives rows first, while coordinates are (x=column, y=row).
    connection = Connection(FakeSocket(b"SET\x32\x28"))
    message = connection.read_message()
    assert message.name == "SET"
    assert message.payload.rows == 50
    assert message.payload.columns == 40


def test_hum_lists_house_coordinates():
    connection = Connection(FakeSocket(b"HUM\x02\x01\x02\x03\x04", chunk=2))
    message = connection.read_message()
    assert message.payload.houses == [(1, 2), (3, 4)]


def test_hme_carries_our_starting_cell():
    connection = Connection(FakeSocket(b"HME\x04\x03"))
    message = connection.read_message()
    assert (message.payload.x, message.payload.y) == (4, 3)


def test_map_decodes_five_byte_cells():
    payload = b"MAP\x02" + bytes([2, 2, 4, 0, 0]) + bytes([4, 1, 0, 3, 0])
    connection = Connection(FakeSocket(payload, chunk=3))
    message = connection.read_message()
    assert message.payload.cells == [(2, 2, 4, 0, 0), (4, 1, 0, 3, 0)]


def test_upd_decodes_like_map():
    connection = Connection(FakeSocket(b"UPD\x01" + bytes([0, 0, 0, 5, 0])))
    message = connection.read_message()
    assert message.name == "UPD"
    assert message.payload.cells == [(0, 0, 0, 5, 0)]


def test_an_empty_update_is_valid():
    # The first player receives MAP and then an UPD with no changes.
    connection = Connection(FakeSocket(b"UPD\x00"))
    assert connection.read_message().payload.cells == []


@pytest.mark.parametrize("name", [b"END", b"BYE"])
def test_end_and_bye_carry_no_payload(name):
    connection = Connection(FakeSocket(name))
    assert connection.read_message().name == name.decode()


def test_an_unknown_command_is_rejected():
    connection = Connection(FakeSocket(b"XYZ\x00"))
    with pytest.raises(ProtocolError):
        connection.read_message()


def test_several_messages_are_read_back_to_back():
    stream = b"SET\x0a\x0a" + b"HUM\x01\x02\x03" + b"END"
    connection = Connection(FakeSocket(stream, chunk=4))
    assert connection.read_message().name == "SET"
    assert connection.read_message().payload.houses == [(2, 3)]
    assert connection.read_message().name == "END"


# ------------------------------------------------------------------ encoding


def test_nme_is_length_prefixed_ascii():
    sock = FakeSocket(b"")
    Connection(sock).send_nme("AI")
    assert bytes(sock.sent) == b"NME\x02AI"


def test_mov_encodes_each_move_as_five_bytes():
    assert encode_mov([(1, 2, 3, 4, 5)]) == b"MOV\x01\x01\x02\x03\x04\x05"


def test_mov_with_several_moves_keeps_their_order():
    encoded = encode_mov([(1, 1, 2, 1, 2), (3, 3, 4, 3, 2)])
    assert encoded == b"MOV\x02" + bytes([1, 1, 2, 1, 2]) + bytes([3, 3, 4, 3, 2])


@pytest.mark.parametrize("bad", [(1, 2, 256, 4, 5), (-1, 2, 3, 4, 5)])
def test_mov_refuses_values_that_do_not_fit_in_a_byte(bad):
    with pytest.raises(ValueError):
        encode_mov([bad])


def test_mov_refuses_a_move_without_five_fields():
    with pytest.raises(ValueError):
        encode_mov([(1, 2, 3)])


# -------------------------------------------------------------- over a socket


def test_a_real_socket_round_trip():
    """Guards against anything that only works with the fake socket."""
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    received = {}

    def serve():
        peer, _ = listener.accept()
        # Dribble the bytes out to force a partial read on the client side.
        for byte in b"SET\x0a\x14":
            peer.sendall(bytes([byte]))
        received["nme"] = peer.recv(16)
        peer.close()

    thread = threading.Thread(target=serve)
    thread.start()
    try:
        connection = Connection.connect("127.0.0.1", port, timeout=5)
        connection.send_nme("Hi")
        message = connection.read_message()
        assert (message.payload.rows, message.payload.columns) == (10, 20)
        connection.close()
    finally:
        thread.join(timeout=5)
        listener.close()
    assert received["nme"] == b"NME\x02Hi"
