#!/usr/bin/env python3
"""Decode submitted VvW MOV commands from a Wireshark/Npcap capture.

Requires tshark (installed with Wireshark). Uses only the Python standard
library for parsing and TCP stream reconstruction.

IMPORTANT: this reports client-to-server MOV commands observed on the wire.
It cannot prove that the server accepted or executed those commands.
"""

from __future__ import annotations

import argparse
import csv
import json
import shutil
import subprocess
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class TcpSegment:
    frame: int
    timestamp: float
    sequence: int
    payload: bytes


@dataclass
class TcpBlock:
    start_sequence: int
    payload: bytearray
    first_frame: int
    first_timestamp: float
    segment_points: list[tuple[int, int, float]]


@dataclass
class MoveRecord:
    connection: str
    tcp_stream: int
    turn_number: int
    move_index: int
    source_x: int
    source_y: int
    creature_count: int
    target_x: int
    target_y: int
    client_ip: str
    client_port: int
    server_ip: str
    server_port: int
    nme_name: str | None
    capture_frame: int | None
    capture_time_epoch: float | None
    observed_as: str = "client_to_server_submitted_MOV"
    server_acceptance: str = "not_proven_by_client_to_server_capture"


def _sequence_offset(sequence: int, base: int) -> int:
    return (sequence - base) & 0xFFFFFFFF


def reassemble_tcp_segments(segments: Iterable[TcpSegment]) -> tuple[list[TcpBlock], int]:
    """Reassemble a direction into contiguous byte blocks.

    Retransmitted/overlapping bytes are de-duplicated. A missing sequence range
    starts a new block, because bytes across a gap must not be fabricated.
    """
    items = list(segments)
    if not items:
        return [], 0

    # Captures are started before the game clients. Use the first observed data
    # sequence as the modular reference point, then sort by sequence space.
    base = min(items, key=lambda item: item.frame).sequence
    items.sort(key=lambda item: (_sequence_offset(item.sequence, base), item.frame))

    blocks: list[TcpBlock] = []
    gaps = 0
    current_end = -1

    for item in items:
        offset = _sequence_offset(item.sequence, base)
        if not blocks or offset > current_end:
            if blocks and offset > current_end:
                gaps += 1
            blocks.append(TcpBlock(
                start_sequence=item.sequence,
                payload=bytearray(item.payload),
                first_frame=item.frame,
                first_timestamp=item.timestamp,
                segment_points=[(0, item.frame, item.timestamp)],
            ))
            current_end = offset + len(item.payload)
            continue

        block = blocks[-1]
        block_start_offset = _sequence_offset(block.start_sequence, base)
        segment_end = offset + len(item.payload)
        block_end = block_start_offset + len(block.payload)
        if segment_end <= block_end:
            # Entire segment is a retransmission or an already-seen overlap.
            continue
        overlap = max(0, block_end - offset)
        append_at = len(block.payload)
        block.payload.extend(item.payload[overlap:])
        block.segment_points.append((append_at, item.frame, item.timestamp))
        current_end = max(current_end, segment_end)

    return blocks, gaps


def decode_client_messages(data: bytes) -> tuple[str | None, list[tuple[int, list[list[int]]]], int]:
    """Find framed NME/MOV messages in one reassembled client->server buffer.

    The stream is parsed by protocol command and length. Scanning for the next
    command allows recovery when the capture begins mid-stream; malformed or
    incomplete frames are counted instead of being guessed.
    """
    index = 0
    nme_name: str | None = None
    move_batches: list[tuple[int, list[list[int]]]] = []
    incomplete = 0

    while index + 4 <= len(data):
        command = data[index:index + 3]

        if command == b"NME":
            name_length = data[index + 3]
            end = index + 4 + name_length
            if end > len(data):
                incomplete += 1
                index += 1
                continue
            try:
                nme_name = data[index + 4:end].decode("utf-8", errors="replace")
            except Exception:
                nme_name = None
            index = end
            continue

        if command == b"MOV":
            count = data[index + 3]
            message_length = 4 + (count * 5)
            end = index + message_length
            if count == 0 or end > len(data):
                incomplete += 1
                index += 1
                continue
            cursor = index + 4
            batch: list[list[int]] = []
            for _ in range(count):
                batch.append([
                    data[cursor],
                    data[cursor + 1],
                    data[cursor + 2],
                    data[cursor + 3],
                    data[cursor + 4],
                ])
                cursor += 5
            move_batches.append((index, batch))
            index = end
            continue

        index += 1

    return nme_name, move_batches, incomplete


def _find_tshark(explicit_path: str | None) -> str:
    candidates = [explicit_path] if explicit_path else []
    candidates.extend([
        shutil.which("tshark"),
        r"C:\Program Files\Wireshark\tshark.exe",
        r"C:\Program Files (x86)\Wireshark\tshark.exe",
    ])
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate).resolve())
    raise FileNotFoundError(
        "tshark was not found. Install Wireshark or pass --tshark PATH."
    )


def _read_tshark_segments(
    tshark: str, capture: Path, port: int
) -> dict[tuple[int, str, int, str, int], list[TcpSegment]]:
    display_filter = f"tcp.port == {port} && tcp.len > 0"
    command = [
        tshark, "-r", str(capture), "-Y", display_filter,
        "-T", "fields", "-E", "header=y", "-E", "separator=/t",
        "-E", "quote=d",
        "-e", "frame.number", "-e", "frame.time_epoch", "-e", "tcp.stream",
        "-e", "ip.src", "-e", "ipv6.src", "-e", "tcp.srcport",
        "-e", "ip.dst", "-e", "ipv6.dst", "-e", "tcp.dstport",
        "-e", "tcp.seq_raw", "-e", "tcp.payload",
    ]
    completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if completed.returncode != 0:
        raise RuntimeError(
            "tshark failed:\n" + completed.stderr.strip() + "\nCommand: " + " ".join(command)
        )

    reader = csv.DictReader(completed.stdout.splitlines(), delimiter="\t", quotechar='"')
    grouped: dict[tuple[int, str, int, str, int], list[TcpSegment]] = defaultdict(list)
    for row in reader:
        try:
            stream = int(row.get("tcp.stream") or "-1")
            frame = int(row.get("frame.number") or "0")
            timestamp = float(row.get("frame.time_epoch") or "0")
            src_ip = row.get("ip.src") or row.get("ipv6.src") or "unknown"
            dst_ip = row.get("ip.dst") or row.get("ipv6.dst") or "unknown"
            src_port = int(row.get("tcp.srcport") or "0")
            dst_port = int(row.get("tcp.dstport") or "0")
            sequence = int(row.get("tcp.seq_raw") or "0")
            payload_hex = (row.get("tcp.payload") or "").replace(":", "").replace(" ", "")
            if not payload_hex:
                continue
            payload = bytes.fromhex(payload_hex)
        except (ValueError, TypeError):
            continue

        key = (stream, src_ip, src_port, dst_ip, dst_port)
        grouped[key].append(TcpSegment(frame, timestamp, sequence, payload))
    return grouped


def analyze_capture(capture: Path, port: int, tshark: str) -> dict:
    grouped = _read_tshark_segments(tshark, capture, port)
    connections: list[dict] = []

    for key, segments in grouped.items():
        stream, src_ip, src_port, dst_ip, dst_port = key
        blocks, gaps = reassemble_tcp_segments(segments)
        decoded_moves: list[dict] = []
        nme_name: str | None = None
        incomplete_frames = 0
        has_nme = False
        first_frame = min((segment.frame for segment in segments), default=0)
        first_timestamp = min((segment.timestamp for segment in segments), default=0.0)

        for block in blocks:
            found_name, found_moves, incomplete = decode_client_messages(bytes(block.payload))
            if found_name is not None:
                has_nme = True
                nme_name = found_name
            for mov_offset, batch in found_moves:
                preceding_points = [point for point in block.segment_points if point[0] <= mov_offset]
                if preceding_points:
                    capture_frame = max(preceding_points, key=lambda point: point[0])[1]
                    capture_timestamp = max(preceding_points, key=lambda point: point[0])[2]
                else:
                    capture_frame = block.first_frame
                    capture_timestamp = block.first_timestamp
                decoded_moves.append({
                    "batch": batch,
                    "capture_frame": capture_frame,
                    "capture_time_epoch": capture_timestamp,
                })
            incomplete_frames += incomplete

        if has_nme or decoded_moves:
            connections.append({
                "tcp_stream": stream,
                "client_ip": src_ip,
                "client_port": src_port,
                "server_ip": dst_ip,
                "server_port": dst_port,
                "nme_name": nme_name,
                "nme_observed": has_nme,
                "first_frame": first_frame,
                "first_timestamp_epoch": first_timestamp,
                "tcp_sequence_gaps": gaps,
                "incomplete_or_zero_count_mov_candidates": incomplete_frames,
                "moves": decoded_moves,
            })

    # NME is the client handshake. Its earliest captured occurrence establishes
    # connection ordering, not species identity or server player-slot identity.
    connections.sort(key=lambda item: (item["first_frame"], item["tcp_stream"]))
    movement_rows: list[dict] = []
    connection_summaries: list[dict] = []
    for connection_index, connection in enumerate(connections, start=1):
        label = f"connection_{connection_index}"
        connection_summaries.append({
            key: value for key, value in connection.items() if key != "moves"
        } | {
            "connection_label": label,
            "mov_message_count": len(connection["moves"]),
            "movement_record_count": sum(len(item["batch"]) for item in connection["moves"]),
            "direction_classification": (
                "confirmed_client_direction_by_NME" if connection["nme_observed"]
                else "suspected_client_direction_by_MOV_only"
            ),
        })
        for turn_number, message in enumerate(connection["moves"], start=1):
            for move_index, values in enumerate(message["batch"], start=1):
                sx, sy, count, dx, dy = values
                movement_rows.append(asdict(MoveRecord(
                    connection=label,
                    tcp_stream=connection["tcp_stream"],
                    turn_number=turn_number,
                    move_index=move_index,
                    source_x=sx,
                    source_y=sy,
                    creature_count=count,
                    target_x=dx,
                    target_y=dy,
                    client_ip=connection["client_ip"],
                    client_port=connection["client_port"],
                    server_ip=connection["server_ip"],
                    server_port=connection["server_port"],
                    nme_name=connection["nme_name"],
                    capture_frame=message["capture_frame"],
                    capture_time_epoch=message["capture_time_epoch"],
                )))

    return {
        "capture_file": str(capture.resolve()),
        "tcp_port_filter": port,
        "capture_packets_with_tcp_payload": sum(len(v) for v in grouped.values()),
        "connections": connection_summaries,
        "movement_record_count": len(movement_rows),
        "movements": movement_rows,
        "interpretation": {
            "what_is_observed": "MOV frames sent from a client endpoint toward the server endpoint",
            "what_is_not_proven": "server acceptance, validation, application, or resulting board state",
            "connection_labels": "ordered by the first captured client handshake/data frame; not species labels or guaranteed server player slots",
            "tcp_reassembly": "segments are reassembled by TCP sequence number; duplicate retransmitted bytes are skipped; gaps are reported",
        },
    }


def write_outputs(result: dict, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "capture_analysis.json").write_text(
        json.dumps(result, indent=2), encoding="utf-8"
    )

    movement_path = output_dir / "movements.csv"
    rows = result["movements"]
    columns = list(rows[0].keys()) if rows else [
        "connection", "tcp_stream", "turn_number", "move_index", "source_x",
        "source_y", "creature_count", "target_x", "target_y", "client_ip",
        "client_port", "server_ip", "server_port", "nme_name", "capture_frame",
        "capture_time_epoch", "observed_as", "server_acceptance",
    ]
    with movement_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)

    lines = [
        "Vampires vs Werewolves TCP capture analysis",
        "============================================",
        f"Capture: {result['capture_file']}",
        f"TCP port filter: {result['tcp_port_filter']}",
        f"TCP payload packets: {result['capture_packets_with_tcp_payload']}",
        f"Connections containing NME or MOV: {len(result['connections'])}",
        f"Decoded MOV movement records: {result['movement_record_count']}",
        "",
    ]
    for connection in result["connections"]:
        lines.append(
            f"{connection['connection_label']}: stream={connection['tcp_stream']} "
            f"{connection['client_ip']}:{connection['client_port']} -> "
            f"{connection['server_ip']}:{connection['server_port']}; "
            f"NME={connection['nme_name']!r}; "
            f"MOV messages={connection['mov_message_count']}; "
            f"movement records={connection['movement_record_count']}; "
            f"TCP gaps={connection['tcp_sequence_gaps']}"
        )
    lines.extend([
        "",
        "CAUTION: these are submitted MOV commands observed on the client-to-server",
        "TCP stream. A packet capture alone does not prove that the server accepted",
        "or executed a move. Do not load these rows as confirmed opponent history.",
    ])
    (output_dir / "capture_report.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pcap", required=True, help="Input .pcap or .pcapng capture")
    parser.add_argument("--port", type=int, required=True, help="Game server TCP port")
    parser.add_argument("--outdir", required=True, help="Directory for CSV/JSON/report outputs")
    parser.add_argument("--tshark", help="Path to tshark.exe if it is not on PATH")
    args = parser.parse_args()

    capture = Path(args.pcap).expanduser().resolve()
    if not capture.is_file():
        parser.error(f"Capture file does not exist: {capture}")
    if not (1 <= args.port <= 65535):
        parser.error("--port must be between 1 and 65535")

    try:
        tshark = _find_tshark(args.tshark)
        result = analyze_capture(capture, args.port, tshark)
        output_dir = Path(args.outdir).expanduser().resolve()
        write_outputs(result, output_dir)
    except (FileNotFoundError, RuntimeError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    print(f"Decoded MOV records: {result['movement_record_count']}")
    print(f"Analysis written to: {output_dir}")
    if not result["movement_record_count"]:
        print(
            "No MOV records decoded. Check capture interface, TCP port, capture timing, "
            "and whether the traffic is encrypted.",
            file=sys.stderr,
        )
        return 1
    print("IMPORTANT: decoded MOV records are submitted commands, not proof of acceptance.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
