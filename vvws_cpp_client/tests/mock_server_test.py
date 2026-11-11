#!/usr/bin/env python3
"""End-to-end smoke test for the C++ TCP client using a tiny mock server."""

import socket
import subprocess
import sys
import threading
import time


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("client closed the connection before the packet was complete")
        data.extend(chunk)
    return bytes(data)


def send_fragmented(sock: socket.socket, packet: bytes) -> None:
    # One byte per send deliberately exercises the client's exact-read logic.
    for byte in packet:
        sock.sendall(bytes([byte]))
        time.sleep(0.001)


def server_thread(listener: socket.socket, errors: list) -> None:
    try:
        conn, _ = listener.accept()
        with conn:
            header = recv_exact(conn, 4)
            assert header[:3] == b"NME", header
            name_length = header[3]
            name = recv_exact(conn, name_length)
            assert name, "client sent an empty name"

            # The test board is intentionally small; the protocol fields are
            # identical to those used by the supplied game server.
            packets = [
                b"SET" + bytes([5, 5]),
                b"HUM" + bytes([1, 0, 0]),
                b"HME" + bytes([2, 2]),
                b"MAP" + bytes([1, 2, 2, 0, 5, 0]),
                b"UPD" + bytes([0]),
            ]
            for packet in packets:
                send_fragmented(conn, packet)

            mov_header = recv_exact(conn, 4)
            assert mov_header[:3] == b"MOV", mov_header
            move_count = mov_header[3]
            assert move_count >= 1, "client must return at least one movement while it has a group"
            moves = recv_exact(conn, move_count * 5)
            sx, sy, count, tx, ty = moves[:5]
            assert (sx, sy) == (2, 2), (sx, sy)
            assert count == 5, count
            assert abs(tx - sx) <= 1 and abs(ty - sy) <= 1, (tx, ty)
            assert (tx, ty) != (sx, sy), (tx, ty)

            send_fragmented(conn, b"END")
            send_fragmented(conn, b"BYE")
    except Exception as exc:  # surfaced in the main thread
        errors.append(exc)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} /path/to/vvws_client", file=sys.stderr)
        return 2

    errors = []
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        host, port = listener.getsockname()
        worker = threading.Thread(target=server_thread, args=(listener, errors), daemon=True)
        worker.start()

        process = subprocess.run(
            [sys.argv[1], host, str(port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
        worker.join(timeout=2)

    if errors:
        raise errors[0]
    if process.returncode != 0:
        print(process.stdout)
        print(process.stderr, file=sys.stderr)
        raise RuntimeError(f"client exited with status {process.returncode}")

    print("PASS: NME, fragmented packet reads, setup, MOV, END and BYE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
